// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2021 Google LLC
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

#include "btf-reader.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>

#include <libabigail/include/abg-tools-utils.h>  // for base_name
#include <libabigail/src/abg-elf-helpers.h>  // for find_section
#include <libabigail/src/abg-symtab-reader.h>  // for symtab_reader

#define m_assert(expr, msg) assert(((void)(msg), (expr)))

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

Structs::Structs(const char* start,
                 std::unique_ptr<abigail::ir::environment> env,
                 const abigail::symtab_reader::symtab_sptr tab,
                 const bool verbose)
    : env_(std::move(env)), tab_(tab), verbose_(verbose) {
  header_ = reinterpret_cast<const btf_header*>(start);
  m_assert(header_->magic == 0xEB9F, "Magic field must be 0xEB9F for BTF");

  type_section_ = reinterpret_cast<const btf_type*>(start + header_->hdr_len +
                                                    header_->type_off);
  str_section_ = start + header_->hdr_len + header_->str_off;

  if (verbose_) {
    PrintHeader();
  }
  BuildTypes();
  if (verbose_) {
    PrintStringSection();
  }
}

// Get the index of the void type, creating one if needed.
size_t Structs::GetVoidIndex() {
  if (!void_type_id_) {
    void_type_id_ = {types_.size()};
    types_.push_back(std::make_unique<Void>(types_));
  }
  return *void_type_id_;
}

// Get the index of the variadic parameter type, creating one if needed.
size_t Structs::GetVariadicIndex() {
  if (!variadic_type_id_) {
    variadic_type_id_ = {types_.size()};
    types_.push_back(std::make_unique<Variadic>(types_));
  }
  return *variadic_type_id_;
}

// Map BTF type index to own index.
//
// If there is no existing mapping for a BTF type, create one pointing to a new
// slot at the end of the array.
size_t Structs::GetIndex(uint32_t btf_index) {
  auto [it, inserted] = type_ids_.insert({btf_index, types_.size()});
  if (inserted)
    types_.push_back(nullptr);
  return it->second;
}

// Translate BTF type id to own type id, for non-parameters.
Id Structs::GetId(uint32_t btf_index) {
  return Id(btf_index ? GetIndex(btf_index) : GetVoidIndex());
}

// Translate BTF type id to own type id, for parameters.
Id Structs::GetParameterId(uint32_t btf_index) {
  return Id(btf_index ? GetIndex(btf_index) : GetVariadicIndex());
}

// The verbose output format closely follows bpftool dump format raw.
static constexpr std::string_view ANON{"(anon)"};

void Structs::PrintHeader() {
  std::cout << "BTF header:\n"
            << "\tmagic " << header_->magic
            << ", version " << static_cast<int>(header_->version)
            << ", flags " << static_cast<int>(header_->flags)
            << ", hdr_len " << header_->hdr_len << "\n"
            << "\ttype_off " << header_->type_off
            << ", type_len " << header_->type_len << "\n"
            << "\tstr_off " << header_->str_off
            << ", str_len " << header_->str_len << "\n";
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
    auto member =
        std::make_unique<Member>(types_, name, GetId(raw_member.type),
                                 static_cast<uint64_t>(offset), bitfield_size);
    auto id = types_.size();
    types_.push_back(std::move(member));
    result.push_back(Id(id));
  }
  return result;
}

// vlen: vector length, the number of enum values
std::vector<std::pair<std::string, int64_t>> Structs::BuildEnums(
    const struct btf_enum* enums, size_t vlen) {
  std::vector<std::pair<std::string, int64_t>> result;
  for (size_t i = 0; i < vlen; ++i) {
    const auto name = GetName(enums[i].name_off);
    const auto value = enums[i].val;
    if (verbose_) {
      std::cout << "\t'" << name << "' val=" << value << '\n';
    }
    result.emplace_back(name, value);
  }
  return result;
}

// vlen: vector length, the number of parameters
std::vector<Parameter> Structs::BuildParams(const struct btf_param* params,
                                            size_t vlen) {
  std::vector<Parameter> result;
  result.reserve(vlen);
  for (size_t i = 0; i < vlen; ++i) {
    const auto name = GetName(params[i].name_off);
    const auto type = params[i].type;
    if (verbose_) {
      std::cout << "\t'" << (name.empty() ? ANON : name)
                << "' type_id=" << type << '\n';
    }
    Parameter parameter{.name_ = name, .typeId_ = GetParameterId(type)};
    result.push_back(std::move(parameter));
  }
  return result;
}

void Structs::BuildTypes() {
  m_assert(!(header_->type_off & (sizeof(uint32_t) - 1)), "Unaligned type_off");
  if (header_->type_len == 0) {
    std::cerr << "No types found";
    return;
  }
  if (verbose_) {
    std::cout << "Type section:\n";
  }

  // Alas, BTF overloads type id 0 to mean both Void (for everything but
  // function parameters) and Variadic (for function parameters). We determine
  // which is intended and create Void and Variadic types on demand.

  // The type section is parsed sequentially and each type's index is its id.
  const char* curr = reinterpret_cast<const char*>(type_section_);
  const char* end = curr + header_->type_len;
  uint32_t btf_index = 1;
  while (curr < end) {
    const btf_type* t = reinterpret_cast<const btf_type*>(curr);
    int type_size = BuildOneType(t, btf_index);
    m_assert(type_size > 0, "Could not identify BTF type");
    curr += type_size;
    ++btf_index;
  }

  BuildSymbols();

  for (const auto& type : types_) {
    m_assert(type, "Undefined type");
    (void)type;
  }
}

int Structs::BuildOneType(const btf_type* t, uint32_t btf_index) {
  const auto kind = BTF_INFO_KIND(t->info);
  const auto vlen = BTF_INFO_VLEN(t->info);
  // Data following the btf_type struct.
  const void* data = reinterpret_cast<const void*>(t + 1);
  m_assert(kind >= 0 && kind < NR_BTF_KINDS, "Unknown BTF kind");
  int type_size = sizeof(struct btf_type);

  if (verbose_)
    std::cout << '[' << btf_index << "] ";
  auto node = [&]() -> std::unique_ptr<Type>& {
    return types_[GetIndex(btf_index)];
  };

  switch (kind) {
    case BTF_KIND_INT: {
      const auto info = *reinterpret_cast<const uint32_t*>(data);
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
      Integer::Encoding encoding =
          is_bool ? Integer::Encoding::BOOLEAN
                  : is_char ? is_signed ? Integer::Encoding::SIGNED_CHARACTER
                                        : Integer::Encoding::UNSIGNED_CHARACTER
                            : is_signed ? Integer::Encoding::SIGNED_INTEGER
                                        : Integer::Encoding::UNSIGNED_INTEGER;
      if (offset)
        std::cerr << "ignoring BTF INT non-zero offset " << offset << '\n';
      node() = std::make_unique<Integer>(types_, name, encoding, bits, t->size);
      type_size += sizeof(uint32_t);
      break;
    }
    case BTF_KIND_PTR: {
      if (verbose_) {
        std::cout << "PTR '" << ANON << "' type_id=" << t->type << '\n';
      }
      node() = std::make_unique<Ptr>(types_, GetId(t->type));
      break;
    }
    case BTF_KIND_TYPEDEF: {
      const auto name = GetName(t->name_off);
      if (verbose_) {
        std::cout << "TYPEDEF '" << name << "' type_id=" << t->type << '\n';
      }
      node() = std::make_unique<Typedef>(types_, name, GetId(t->type));
      break;
    }
    case BTF_KIND_VOLATILE:
    case BTF_KIND_CONST:
    case BTF_KIND_RESTRICT: {
      const auto qualifier = kind == BTF_KIND_CONST
                             ? QualifierKind::CONST
                             : kind == BTF_KIND_VOLATILE
                             ? QualifierKind::VOLATILE
                             : QualifierKind::RESTRICT;
      if (verbose_) {
        std::cout << (kind == BTF_KIND_CONST ? "CONST"
                      : kind == BTF_KIND_VOLATILE ? "VOLATILE"
                      : "RESTRICT")
                  << " '" << ANON << "' type_id=" << t->type << '\n';
      }
      node() = std::make_unique<Qualifier>(types_, qualifier, GetId(t->type));
      break;
    }
    case BTF_KIND_ARRAY: {
      const auto* array = reinterpret_cast<const struct btf_array*>(data);
      if (verbose_) {
        std::cout << "ARRAY '" << ANON << "'"
                  << " type_id=" << array->type
                  << " index_type_id=" << array->index_type
                  << " nr_elems=" << array->nelems
                  << '\n';
      }
      node() =
          std::make_unique<Array>(types_, GetId(array->type), array->nelems);
      type_size += sizeof(struct btf_array);
      break;
    }
    case BTF_KIND_STRUCT:
    case BTF_KIND_UNION: {
      const auto structUnionKind = kind == BTF_KIND_STRUCT
                             ? StructUnionKind::STRUCT
                             : StructUnionKind::UNION;
      const auto name = GetName(t->name_off);
      const bool kflag = BTF_INFO_KFLAG(t->info);
      if (verbose_) {
        std::cout << (kind == BTF_KIND_STRUCT ? "STRUCT" : "UNION")
                  << " '" << (name.empty() ? ANON : name) << "'"
                  << " size=" << t->size
                  << " vlen=" << vlen << '\n';
      }
      const auto* btf_members = reinterpret_cast<const btf_member*>(data);
      const auto members = BuildMembers(kflag, btf_members, vlen);
      node() = std::make_unique<StructUnion>(types_, name, structUnionKind,
                                             t->size, members);
      type_size += vlen * sizeof(struct btf_member);
      break;
    }
    case BTF_KIND_ENUM: {
      const auto name = GetName(t->name_off);
      if (verbose_) {
        std::cout << "ENUM '" << (name.empty() ? ANON : name) << "'"
                  << " size=" << t->size
                  << " vlen=" << vlen
                  << '\n';
      }
      const auto* enums = reinterpret_cast<const struct btf_enum*>(data);
      const auto enumerators = BuildEnums(enums, vlen);
      // BTF only considers structs and unions as forward-declared types, and
      // does not include forward-declared enums. They are treated as
      // BTF_KIND_ENUMs with vlen set to zero.
      if (vlen) {
        node() =
            std::make_unique<Enumeration>(types_, name, t->size, enumerators);
      } else {
        // BTF actually provides size (4), but it's meaningless.
        node() = std::make_unique<ForwardDeclaration>(
            types_, name, ForwardDeclarationKind::ENUM);
      }
      type_size += vlen * sizeof(struct btf_enum);
      break;
    }
    case BTF_KIND_FWD: {
      const auto name = GetName(t->name_off);
      const auto forwardKind = BTF_INFO_KFLAG(t->info)
                               ? ForwardDeclarationKind::UNION
                               : ForwardDeclarationKind::STRUCT;
      if (verbose_) {
        std::cout << "FWD '" << name << "' fwd_kind=" << forwardKind << '\n';
      }
      node() = std::make_unique<ForwardDeclaration>(types_, name, forwardKind);
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

      bool inserted = btf_symbol_types_.insert({name, GetId(t->type)}).second;
      m_assert(inserted, "Insertion failed, duplicate found in symbol map");
      (void)inserted;
      break;
    }
    case BTF_KIND_FUNC_PROTO: {
      const auto* params = reinterpret_cast<const btf_param*>(data);
      if (verbose_) {
        std::cout << "FUNC_PROTO '" << ANON << "'"
                  << " ret_type_id=" << t->type
                  << " vlen=" << vlen
                  << '\n';
      }
      const auto parameters = BuildParams(params, vlen);
      node() = std::make_unique<Function>(types_, GetId(t->type), parameters);
      type_size += vlen * sizeof(struct btf_param);
      break;
    }
    case BTF_KIND_VAR: {
      // NOTE: not yet encountered in the wild
      const auto* variable = reinterpret_cast<const struct btf_var*>(data);
      const auto name = GetName(t->name_off);
      const auto linkage = VariableLinkage(variable->linkage);
      if (verbose_) {
        // NOTE: The odd comma is to match bpftool dump.
        std::cout << "VAR type_id=" << t->type
                  << ", linkage=" << linkage
                  << '\n';
      }

      bool inserted = btf_symbol_types_.insert({name, GetId(t->type)}).second;
      m_assert(inserted, "Insertion failed, duplicate found in symbol map");
      (void)inserted;

      type_size += sizeof(struct btf_var);
      break;
    }
    case BTF_KIND_DATASEC: {
      if (verbose_) {
        std::cout << "DATASEC\n";
      }
      // Just skip BTF DATASEC entries. They partially duplicate ELF symbol
      // table information, if they exist at all.
      type_size += vlen * sizeof(struct btf_var_secinfo);
      break;
    }
  }
  return type_size;
}

std::string Structs::GetName(uint32_t name_off) {
  m_assert(name_off < header_->str_len,
           "The name offset exceeds the section length");
  const char* section_end = str_section_ + header_->str_len;
  const char* name_begin = str_section_ + name_off;
  const char* name_end = std::find(name_begin, section_end, '\0');
  m_assert(name_end < section_end,
           "The name continues past the string section limit");
  return {name_begin, static_cast<size_t>(name_end - name_begin)};
}

void Structs::PrintStringSection() {
  std::cout << "String section:\n";
  const char* curr = str_section_;
  const char* limit = str_section_ + header_->str_len;
  while (curr < limit) {
    const char* pos = std::find(curr, limit, '\0');
    m_assert(pos < limit, "Error reading the string section");
    std::cout << ' ' << curr;
    curr = pos + 1;
  }
  std::cout << '\n';
}

void Structs::BuildSymbols() {
  const auto filter = [&]() {
    auto filter = tab_->make_filter();
    filter.set_public_symbols();
    return filter;
  }();
  std::map<std::string, Id> elf_symbols;
  for (const auto& symbol :
           abigail::symtab_reader::filtered_symtab(*tab_, filter)) {
    std::optional<Id> type_id;
    const auto& symbol_name = symbol->get_name();
    const auto& main_symbol_name = symbol->get_main_symbol()->get_name();
    auto it = btf_symbol_types_.find(main_symbol_name);
    if (it == btf_symbol_types_.end()) {
      // missing BTF information is tracked explicitly
      std::cerr << "ELF symbol " << std::quoted(symbol_name, '\'');
      if (symbol_name != main_symbol_name)
        std::cerr << " (aliased to " << std::quoted(main_symbol_name, '\'')
                  << ')';
      std::cerr << " BTF info missing\n";
    } else {
      type_id = {it->second};
    }
    auto elf_symbol_id = types_.size();
    types_.push_back(std::make_unique<ElfSymbol>(types_, symbol, type_id));
    auto key = symbol_name + '@' + symbol->get_version().str();
    elf_symbols.emplace(std::move(key), elf_symbol_id);
  }
  symbols_index_ = types_.size();
  types_.push_back(std::make_unique<Symbols>(types_, elf_symbols));
}

class ElfHandle {
 public:
  ElfHandle(const std::string& path) : dwfl_(nullptr, dwfl_end) {
    std::string name;
    abigail::tools_utils::base_name(path, name);

    elf_version(EV_CURRENT);

    dwfl_ = std::unique_ptr<Dwfl, decltype(&dwfl_end)>(
        dwfl_begin(&offline_callbacks_), dwfl_end);
    auto dwfl_module =
        dwfl_report_offline(dwfl_.get(), name.c_str(), path.c_str(), -1);
    GElf_Addr bias;
    elf_handle_ = dwfl_module_getelf(dwfl_module, &bias);
  }

  // Conversion operator to act as a drop-in replacement for Elf*
  operator Elf*() const { return elf_handle_; }

  Elf* get() const { return elf_handle_; }

 private:
  // Dwfl owns all our data, hence only keep track of this
  std::unique_ptr<Dwfl, decltype(&dwfl_end)> dwfl_;
  Elf* elf_handle_;

  Dwfl_Callbacks offline_callbacks_;
};

std::unique_ptr<Structs> ReadFile(const std::string& path, bool verbose) {
  using abigail::symtab_reader::symtab;

  ElfHandle elf(path);
  m_assert(elf.get() != nullptr, "Could not get elf handle from file.");

  Elf_Scn* btf_section =
      abigail::elf_helpers::find_section(elf, ".BTF", SHT_PROGBITS);
  m_assert(btf_section != nullptr,
           "The given file does not have a BTF section");
  Elf_Data* elf_data = elf_rawdata(btf_section, 0);
  m_assert(elf_data != nullptr, "The BTF section is invalid");
  const char* btf_start = static_cast<char*>(elf_data->d_buf);

  auto env = std::make_unique<abigail::ir::environment>();
  auto tab = symtab::load(elf, env.get());

  return std::make_unique<Structs>(
      btf_start, std::move(env), std::move(tab), verbose);
}

}  // end namespace btf
}  // end namespace stg
