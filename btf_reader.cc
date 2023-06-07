// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2022 Google LLC
//
// Licensed under the Apache License v2.0 with LLVM Exceptions (the
// "License"); you may not use this file except in compliance with the
// License.  You may obtain a copy of the License at
//
//     https://llvm.org/LICENSE.txt
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: Maria Teguiani
// Author: Giuliano Procida
// Author: Ignes Simeonova
// Author: Aleksei Vetrov

#include "btf_reader.h"

#include <fcntl.h>
#include <libelf.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>

#include "elf_loader.h"
#include "error.h"
#include "file_descriptor.h"

namespace stg {

namespace btf {

static constexpr std::array<std::string_view, 3> kVarLinkage = {
    "static",
    "global-alloc",
    "global-extern",  // NOTE: bpftool currently says "(unknown)"
};

static constexpr std::array<std::string_view, 3> kFunLinkage = {
    "static",
    "global",
    "extern",
};

std::string_view VariableLinkage(size_t ix) {
  return ix < kVarLinkage.size() ? kVarLinkage[ix] : "(unknown)";
}

std::string_view FunctionLinkage(size_t ix) {
  return ix < kFunLinkage.size() ? kFunLinkage[ix] : "(unknown)";
}

bool Structs::MemoryRange::Empty() const {
  return start == limit;
}

template <typename T>
const T* Structs::MemoryRange::Pull(size_t count) {
  const char* saved = start;
  start += sizeof(T) * count;
  Check(start <= limit) << "type data extends past end of type section";
  return reinterpret_cast<const T*>(saved);
}

Structs::Structs(Graph& graph, const bool verbose)
    : graph_(graph), verbose_(verbose) { }

// Get the index of the void type, creating one if needed.
Id Structs::GetVoid() {
  if (!void_)
    void_ = {graph_.Add<Void>()};
  return *void_;
}

// Get the index of the variadic parameter type, creating one if needed.
Id Structs::GetVariadic() {
  if (!variadic_)
    variadic_ = {graph_.Add<Variadic>()};
  return *variadic_;
}

// Map BTF type index to own index.
//
// If there is no existing mapping for a BTF type, create one pointing to a new
// slot at the end of the array.
Id Structs::GetIdRaw(uint32_t btf_index) {
  auto [it, inserted] = btf_type_ids_.insert({btf_index, Id(0)});
  if (inserted)
    it->second = graph_.Allocate();
  return it->second;
}

// Translate BTF type id to own type id, for non-parameters.
Id Structs::GetId(uint32_t btf_index) {
  return btf_index ? GetIdRaw(btf_index) : GetVoid();
}

// Translate BTF type id to own type id, for parameters.
Id Structs::GetParameterId(uint32_t btf_index) {
  return btf_index ? GetIdRaw(btf_index) : GetVariadic();
}

// The verbose output format closely follows bpftool dump format raw.
static constexpr std::string_view ANON{"(anon)"};

Id Structs::Process(std::string_view btf_data) {
  Check(sizeof(btf_header) <= btf_data.size())
      << "BTF section too small for header";
  const btf_header* header =
      reinterpret_cast<const btf_header*>(btf_data.data());
  if (verbose_)
    PrintHeader(header);
  Check(header->magic == 0xEB9F) << "Magic field must be 0xEB9F for BTF";

  const char* header_limit = btf_data.begin() + header->hdr_len;
  const char* type_start = header_limit + header->type_off;
  const char* type_limit = type_start + header->type_len;
  const char* string_start = header_limit + header->str_off;
  const char* string_limit = string_start + header->str_len;

  Check(btf_data.begin() + sizeof(btf_header) <= header_limit)
      << "header exceeds length";
  Check(header_limit <= type_start) << "type section overlaps header";
  Check(type_start <= type_limit) << "type section ill-formed";
  Check(!(header->type_off & (sizeof(uint32_t) - 1)))
      << "misaligned type section";
  Check(type_limit <= string_start)
      << "string section does not follow type section";
  Check(string_start <= string_limit) << "string section ill-formed";
  Check(string_limit <= btf_data.end())
      << "string section extends beyond end of BTF data";

  const MemoryRange type_section{type_start, type_limit};
  string_section_ = MemoryRange{string_start, string_limit};
  const Id root = BuildTypes(type_section);
  if (verbose_)
    PrintStrings(string_section_);
  return root;
}

void Structs::PrintHeader(const btf_header* header) const {
  std::cout << "BTF header:\n"
            << "\tmagic " << header->magic
            << ", version " << static_cast<int>(header->version)
            << ", flags " << static_cast<int>(header->flags)
            << ", hdr_len " << header->hdr_len << "\n"
            << "\ttype_off " << header->type_off
            << ", type_len " << header->type_len << "\n"
            << "\tstr_off " << header->str_off
            << ", str_len " << header->str_len << "\n";
}

// vlen: vector length, the number of struct/union members
std::vector<Id> Structs::BuildMembers(
    bool kflag, const btf_member* members, size_t vlen) {
  std::vector<Id> result;
  for (size_t i = 0; i < vlen; ++i) {
    const auto& raw_member = members[i];
    const auto name = GetName(raw_member.name_off);
    const auto raw_offset = raw_member.offset;
    const auto offset = kflag ? BTF_MEMBER_BIT_OFFSET(raw_offset) : raw_offset;
    const auto bitfield_size = kflag ? BTF_MEMBER_BITFIELD_SIZE(raw_offset) : 0;
    if (verbose_) {
      std::cout << "\t'" << (name.empty() ? ANON : name) << '\''
                << " type_id=" << raw_member.type
                << " bits_offset=" << offset;
      if (bitfield_size)
        std::cout << " bitfield_size=" << bitfield_size;
      std::cout << '\n';
    }
    result.push_back(
        graph_.Add<Member>(name, GetId(raw_member.type),
                           static_cast<uint64_t>(offset), bitfield_size));
  }
  return result;
}

// vlen: vector length, the number of enum values
std::vector<std::pair<std::string, int64_t>> Structs::BuildEnums(
    bool is_signed, const struct btf_enum* enums, size_t vlen) {
  std::vector<std::pair<std::string, int64_t>> result;
  for (size_t i = 0; i < vlen; ++i) {
    const auto name = GetName(enums[i].name_off);
    const uint32_t unsigned_value = enums[i].val;
    if (is_signed) {
      const int32_t signed_value = unsigned_value;
      if (verbose_) {
        std::cout << "\t'" << name << "' val=" << signed_value << '\n';
      }
      result.emplace_back(name, static_cast<int64_t>(signed_value));
    } else {
      if (verbose_) {
        std::cout << "\t'" << name << "' val=" << unsigned_value << '\n';
      }
      result.emplace_back(name, static_cast<int64_t>(unsigned_value));
    }
  }
  return result;
}

// vlen: vector length, the number of parameters
std::vector<Id> Structs::BuildParams(const struct btf_param* params,
                                     size_t vlen) {
  std::vector<Id> result;
  result.reserve(vlen);
  for (size_t i = 0; i < vlen; ++i) {
    const auto name = GetName(params[i].name_off);
    const auto type = params[i].type;
    if (verbose_) {
      std::cout << "\t'" << (name.empty() ? ANON : name)
                << "' type_id=" << type << '\n';
    }
    result.push_back(GetParameterId(type));
  }
  return result;
}

Id Structs::BuildTypes(MemoryRange memory) {
  if (verbose_) {
    std::cout << "Type section:\n";
  }

  // Alas, BTF overloads type id 0 to mean both Void (for everything but
  // function parameters) and Variadic (for function parameters). We determine
  // which is intended and create Void and Variadic types on demand.

  // The type section is parsed sequentially and each type's index is its id.
  uint32_t btf_index = 1;
  while (!memory.Empty()) {
    const auto* t = memory.Pull<struct btf_type>();
    BuildOneType(t, btf_index, memory);
    ++btf_index;
  }

  return BuildSymbols();
}

void Structs::BuildOneType(const btf_type* t, uint32_t btf_index,
                           MemoryRange& memory) {
  const auto kind = BTF_INFO_KIND(t->info);
  const auto vlen = BTF_INFO_VLEN(t->info);
  Check(kind < NR_BTF_KINDS) << "Unknown BTF kind: " << static_cast<int>(kind);

  if (verbose_)
    std::cout << '[' << btf_index << "] ";
  // delay allocation of node id as some BTF nodes are skipped
  auto id = [&]() {
    return GetIdRaw(btf_index);
  };

  switch (kind) {
    case BTF_KIND_INT: {
      const auto info = *memory.Pull<uint32_t>();
      const auto name = GetName(t->name_off);
      const auto raw_encoding = BTF_INT_ENCODING(info);
      const auto offset = BTF_INT_OFFSET(info);
      const auto bits = BTF_INT_BITS(info);
      const auto is_bool = raw_encoding & BTF_INT_BOOL;
      const auto is_signed = raw_encoding & BTF_INT_SIGNED;
      const auto is_char = raw_encoding & BTF_INT_CHAR;
      if (verbose_) {
        std::cout << "INT '" << name << "'"
                  << " size=" << t->size
                  << " bits_offset=" << offset
                  << " nr_bits=" << bits
                  << " encoding=" << (is_bool ? "BOOL"
                                      : is_signed ? "SIGNED"
                                      : is_char ? "CHAR"
                                      : "(none)")
                  << '\n';
      }
      Primitive::Encoding encoding =
          is_bool ? Primitive::Encoding::BOOLEAN
                : is_char ? is_signed ? Primitive::Encoding::SIGNED_CHARACTER
                                      : Primitive::Encoding::UNSIGNED_CHARACTER
                          : is_signed ? Primitive::Encoding::SIGNED_INTEGER
                                      : Primitive::Encoding::UNSIGNED_INTEGER;
      if (offset)
        Die() << "BTF INT non-zero offset " << offset << '\n';
      graph_.Set<Primitive>(id(), name, encoding, bits, t->size);
      break;
    }
    case BTF_KIND_PTR: {
      if (verbose_) {
        std::cout << "PTR '" << ANON << "' type_id=" << t->type << '\n';
      }
      graph_.Set<PointerReference>(id(), PointerReference::Kind::POINTER,
                                   GetId(t->type));
      break;
    }
    case BTF_KIND_TYPEDEF: {
      const auto name = GetName(t->name_off);
      if (verbose_) {
        std::cout << "TYPEDEF '" << name << "' type_id=" << t->type << '\n';
      }
      graph_.Set<Typedef>(id(), name, GetId(t->type));
      break;
    }
    case BTF_KIND_VOLATILE:
    case BTF_KIND_CONST:
    case BTF_KIND_RESTRICT: {
      const auto qualifier = kind == BTF_KIND_CONST
                             ? Qualifier::CONST
                             : kind == BTF_KIND_VOLATILE
                             ? Qualifier::VOLATILE
                             : Qualifier::RESTRICT;
      if (verbose_) {
        std::cout << (kind == BTF_KIND_CONST ? "CONST"
                      : kind == BTF_KIND_VOLATILE ? "VOLATILE"
                      : "RESTRICT")
                  << " '" << ANON << "' type_id=" << t->type << '\n';
      }
      graph_.Set<Qualified>(id(), qualifier, GetId(t->type));
      break;
    }
    case BTF_KIND_ARRAY: {
      const auto* array = memory.Pull<struct btf_array>();
      if (verbose_) {
        std::cout << "ARRAY '" << ANON << "'"
                  << " type_id=" << array->type
                  << " index_type_id=" << array->index_type
                  << " nr_elems=" << array->nelems
                  << '\n';
      }
      graph_.Set<Array>(id(), array->nelems, GetId(array->type));
      break;
    }
    case BTF_KIND_STRUCT:
    case BTF_KIND_UNION: {
      const auto struct_union_kind = kind == BTF_KIND_STRUCT
                                     ? StructUnion::Kind::STRUCT
                                     : StructUnion::Kind::UNION;
      const auto name = GetName(t->name_off);
      const bool kflag = BTF_INFO_KFLAG(t->info);
      if (verbose_) {
        std::cout << (kind == BTF_KIND_STRUCT ? "STRUCT" : "UNION")
                  << " '" << (name.empty() ? ANON : name) << "'"
                  << " size=" << t->size
                  << " vlen=" << vlen << '\n';
      }
      const auto* btf_members = memory.Pull<struct btf_member>(vlen);
      const auto members = BuildMembers(kflag, btf_members, vlen);
      graph_.Set<StructUnion>(id(), struct_union_kind, name, t->size,
                              std::vector<Id>(), std::vector<Id>(), members);
      break;
    }
    case BTF_KIND_ENUM: {
      const auto name = GetName(t->name_off);
      const bool is_signed = BTF_INFO_KFLAG(t->info);
      if (verbose_) {
        std::cout << "ENUM '" << (name.empty() ? ANON : name) << "'"
                  << " encoding=" << (is_signed ? "SIGNED" : "UNSIGNED")
                  << " size=" << t->size
                  << " vlen=" << vlen
                  << '\n';
      }
      const auto* enums = memory.Pull<struct btf_enum>(vlen);
      const auto enumerators = BuildEnums(is_signed, enums, vlen);
      // BTF only considers structs and unions as forward-declared types, and
      // does not include forward-declared enums. They are treated as
      // BTF_KIND_ENUMs with vlen set to zero.
      if (vlen) {
        graph_.Set<Enumeration>(id(), name, t->size, enumerators);
      } else {
        // BTF actually provides size (4), but it's meaningless.
        graph_.Set<Enumeration>(id(), name);
      }
      break;
    }
    case BTF_KIND_FWD: {
      const auto name = GetName(t->name_off);
      const auto struct_union_kind = BTF_INFO_KFLAG(t->info)
                                     ? StructUnion::Kind::UNION
                                     : StructUnion::Kind::STRUCT;
      if (verbose_) {
        std::cout << "FWD '" << name << "' fwd_kind=" << struct_union_kind
                  << '\n';
      }
      graph_.Set<StructUnion>(id(), struct_union_kind, name);
      break;
    }
    case BTF_KIND_FUNC: {
      const auto name = GetName(t->name_off);
      const auto linkage = FunctionLinkage(vlen);
      if (verbose_) {
        std::cout << "FUNC '" << name << "'"
                  << " type_id=" << t->type
                  << " linkage=" << linkage
                  << '\n';
      }

      graph_.Set<ElfSymbol>(id(), name, std::nullopt, true,
                            ElfSymbol::SymbolType::FUNCTION,
                            ElfSymbol::Binding::GLOBAL,
                            ElfSymbol::Visibility::DEFAULT,
                            std::nullopt,
                            std::nullopt,
                            GetId(t->type),
                            std::nullopt);
      bool inserted = btf_symbols_.insert({name, GetIdRaw(btf_index)}).second;
      Check(inserted) << "duplicate symbol " << name;
      break;
    }
    case BTF_KIND_FUNC_PROTO: {
      const auto* params = memory.Pull<struct btf_param>(vlen);
      if (verbose_) {
        std::cout << "FUNC_PROTO '" << ANON << "'"
                  << " ret_type_id=" << t->type
                  << " vlen=" << vlen
                  << '\n';
      }
      const auto parameters = BuildParams(params, vlen);
      graph_.Set<Function>(id(), GetId(t->type), parameters);
      break;
    }
    case BTF_KIND_VAR: {
      // NOTE: global variables are not yet emitted by pahole -J
      const auto* variable = memory.Pull<struct btf_var>();
      const auto name = GetName(t->name_off);
      const auto linkage = VariableLinkage(variable->linkage);
      if (verbose_) {
        // NOTE: The odd comma is to match bpftool dump.
        std::cout << "VAR '" << name << "'"
                  << " type_id=" << t->type
                  << ", linkage=" << linkage
                  << '\n';
      }

      graph_.Set<ElfSymbol>(id(), name, std::nullopt, true,
                            ElfSymbol::SymbolType::OBJECT,
                            ElfSymbol::Binding::GLOBAL,
                            ElfSymbol::Visibility::DEFAULT,
                            std::nullopt,
                            std::nullopt,
                            GetId(t->type),
                            std::nullopt);
      bool inserted = btf_symbols_.insert({name, GetIdRaw(btf_index)}).second;
      Check(inserted) << "duplicate symbol " << name;
      break;
    }
    case BTF_KIND_DATASEC: {
      if (verbose_) {
        std::cout << "DATASEC\n";
      }
      // Just skip BTF DATASEC entries. They partially duplicate ELF symbol
      // table information, if they exist at all.
      memory.Pull<struct btf_var_secinfo>(vlen);
      break;
    }
    default: {
      Die() << "Unhandled BTF kind: " << static_cast<int>(kind);
      break;
    }
  }
}

std::string Structs::GetName(uint32_t name_off) {
  const char* name_begin = string_section_.start + name_off;
  const char* const limit = string_section_.limit;
  Check(name_begin < limit) << "name offset exceeds string section length";
  const char* name_end = std::find(name_begin, limit, '\0');
  Check(name_end < limit) << "name continues past the string section limit";
  return {name_begin, static_cast<size_t>(name_end - name_begin)};
}

void Structs::PrintStrings(MemoryRange memory) {
  std::cout << "String section:\n";
  while (!memory.Empty()) {
    const char* position = std::find(memory.start, memory.limit, '\0');
    Check(position < memory.limit) << "Error reading the string section";
    const size_t size = position - memory.start;
    std::cout << ' ' << std::string_view{memory.Pull<char>(size + 1), size};
  }
  std::cout << '\n';
}

Id Structs::BuildSymbols() {
  return graph_.Add<Symbols>(btf_symbols_);
}

Id ReadFile(Graph& graph, const std::string& path, bool verbose) {
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  struct ElfDeleter {
    void operator()(Elf* elf) { elf_end(elf); }
  };
  FileDescriptor fd(path.c_str(), O_RDONLY);
  std::unique_ptr<Elf, ElfDeleter> elf(
      elf_begin(fd.Value(), ELF_C_READ, nullptr));
  if (!elf) {
    const int error_code = elf_errno();
    const char* error = elf_errmsg(error_code);
    if (error != nullptr) {
      Die() << "elf_begin returned error: " << error;
    } else {
      Die() << "elf_begin returned error: " << error_code;
    }
  }
  elf::ElfLoader loader(elf.get());
  return Structs(graph, verbose).Process(loader.GetBtfRawData());
}

}  // namespace btf

}  // namespace stg
