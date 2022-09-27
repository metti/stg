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
// Author: Aleksei Vetrov

#include "elf_loader.h"

#include <fcntl.h>
#include <unistd.h>

#include <elf.h>
#include <gelf.h>
#include <libelf.h>

#include <cstddef>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#include "error.h"
#include "stg.h"

namespace stg {
namespace elf {

namespace {

SymbolTableEntry::SymbolType ParseSymbolType(unsigned char symbol_type) {
  switch (symbol_type) {
    case STT_NOTYPE:
      return SymbolTableEntry::SymbolType::NOTYPE;
    case STT_OBJECT:
      return SymbolTableEntry::SymbolType::OBJECT;
    case STT_FUNC:
      return SymbolTableEntry::SymbolType::FUNCTION;
    case STT_SECTION:
      return SymbolTableEntry::SymbolType::SECTION;
    case STT_FILE:
      return SymbolTableEntry::SymbolType::FILE;
    case STT_COMMON:
      return SymbolTableEntry::SymbolType::COMMON;
    case STT_TLS:
      return SymbolTableEntry::SymbolType::TLS;
    case STT_GNU_IFUNC:
      return SymbolTableEntry::SymbolType::GNU_IFUNC;
    default:
      Die() << "Unknown ELF symbol type: " << symbol_type;
  }
}

SymbolTableEntry::Binding ParseSymbolBinding(unsigned char binding) {
  switch (binding) {
    case STB_LOCAL:
      return SymbolTableEntry::Binding::LOCAL;
    case STB_GLOBAL:
      return SymbolTableEntry::Binding::GLOBAL;
    case STB_WEAK:
      return SymbolTableEntry::Binding::WEAK;
    case STB_GNU_UNIQUE:
      return SymbolTableEntry::Binding::GNU_UNIQUE;
    default:
      Die() << "Unknown ELF symbol binding: " << binding;
  }
}

SymbolTableEntry::Visibility ParseSymbolVisibility(unsigned char visibility) {
  switch (visibility) {
    case STV_DEFAULT:
      return SymbolTableEntry::Visibility::DEFAULT;
    case STV_INTERNAL:
      return SymbolTableEntry::Visibility::INTERNAL;
    case STV_HIDDEN:
      return SymbolTableEntry::Visibility::HIDDEN;
    case STV_PROTECTED:
      return SymbolTableEntry::Visibility::PROTECTED;
    default:
      Die() << "Unknown ELF symbol visibility: " << visibility;
  }
}

SymbolTableEntry::ValueType ParseSymbolValueType(Elf64_Section section_index) {
  switch (section_index) {
    case SHN_UNDEF:
      return SymbolTableEntry::ValueType::UNDEFINED;
    case SHN_ABS:
      return SymbolTableEntry::ValueType::ABSOLUTE;
    case SHN_COMMON:
      return SymbolTableEntry::ValueType::COMMON;
    default:
      return SymbolTableEntry::ValueType::RELATIVE_TO_SECTION;
  }
}

std::string ElfHeaderTypeToString(unsigned char elf_header_type) {
  switch (elf_header_type) {
    case ET_NONE:
      return "none";
    case ET_REL:
      return "relocatable";
    case ET_EXEC:
      return "executable";
    case ET_DYN:
      return "shared object";
    case ET_CORE:
      return "coredump";
    default:
      return "unknown (type = " + std::to_string(elf_header_type) + ')';
  }
}

std::string ElfSectionTypeToString(Elf64_Word elf_section_type) {
  switch (elf_section_type) {
    case SHT_SYMTAB:
      return "symtab";
    case SHT_DYNSYM:
      return "dynsym";
    case SHT_GNU_verdef:
      return "GNU_verdef";
    case SHT_GNU_verneed:
      return "GNU_verneed";
    case SHT_GNU_versym:
      return "GNU_versym";
    default:
      return "unknown (type = " + std::to_string(elf_section_type) + ')';
  }
}

std::vector<Elf_Scn*> GetSectionsIf(
    Elf* elf, std::function<bool(const GElf_Shdr&)> predicate) {
  std::vector<Elf_Scn*> result;
  Elf_Scn* section = nullptr;
  GElf_Shdr header;
  while ((section = elf_nextscn(elf, section)) != nullptr) {
    Check(gelf_getshdr(section, &header) != nullptr)
        << "could not get ELF section header";
    if (predicate(header)) {
      result.push_back(section);
    }
  }
  return result;
}

std::vector<Elf_Scn*> GetSectionsByName(Elf* elf, const std::string& name) {
  size_t shdr_strtab_index;
  Check(elf_getshdrstrndx(elf, &shdr_strtab_index) == 0)
      << "could not get ELF section header string table index";
  return GetSectionsIf(elf, [&](const GElf_Shdr& header) {
    const auto* section_name =
        elf_strptr(elf, shdr_strtab_index, header.sh_name);
    return section_name != nullptr && section_name == name;
  });
}

Elf_Scn* MaybeGetSectionByName(Elf* elf, const std::string& name) {
  const auto sections = GetSectionsByName(elf, name);
  if (sections.empty()) {
    return nullptr;
  }
  Check(sections.size() == 1)
      << "multiple sections found with name '" << name << "'";
  return sections[0];
}

Elf_Scn* GetSectionByName(Elf* elf, const std::string& name) {
  Elf_Scn* section = MaybeGetSectionByName(elf, name);
  Check(section != nullptr) << "no section found with name '" << name << "'";
  return section;
}

Elf_Scn* MaybeGetSectionByType(Elf* elf, Elf64_Word type) {
  auto sections = GetSectionsIf(
      elf, [&](const GElf_Shdr& header) { return header.sh_type == type; });
  if (sections.empty()) {
    return nullptr;
  }
  Check(sections.size() == 1) << "multiple sections found with type " << type;
  return sections[0];
}

Elf_Scn* GetSectionByType(Elf* elf, Elf64_Word type) {
  Elf_Scn* section = MaybeGetSectionByType(elf, type);
  Check(section != nullptr) << "no section found with type " << type;
  return section;
}

struct SectionInfo {
  GElf_Shdr header;
  Elf_Data* data;
};

SectionInfo GetSectionInfo(Elf_Scn* section) {
  size_t index = elf_ndxscn(section);
  GElf_Shdr section_header;
  Check(gelf_getshdr(section, &section_header) != nullptr)
      << "failed to read section (index = " << index << ") header";
  Elf_Data* data = elf_getdata(section, 0);
  Check(data != nullptr) << "section (index = " << index << ") data is invalid";
  return {section_header, data};
}

size_t GetNumberOfEntries(const GElf_Shdr& section_header) {
  Check(section_header.sh_entsize != 0)
      << "zero table entity size is unexpected for section "
      << ElfSectionTypeToString(section_header.sh_type);
  return section_header.sh_size / section_header.sh_entsize;
}

std::string_view GetString(Elf* elf, uint32_t section, size_t offset) {
  const auto name = elf_strptr(elf, section, offset);

  Check(name != nullptr) << "string was not found (section: " << section
                         << ", offset: " << offset << ")";
  return name;
}

Elf_Scn* GetSymbolTableSection(Elf* elf, bool verbose) {
  GElf_Ehdr elf_header;
  Check(gelf_getehdr(elf, &elf_header) != nullptr)
      << "could not get ELF header";

  if (verbose) {
    std::cout << "ELF type: " << ElfHeaderTypeToString(elf_header.e_type)
              << '\n';
  }
  // TODO: check if vmlinux symbol table type matches ELF type
  // same way as other binaries
  if (elf_header.e_type == ET_REL) {
    return GetSectionByType(elf, SHT_SYMTAB);
  } else if (elf_header.e_type == ET_DYN || elf_header.e_type == ET_EXEC) {
    return GetSectionByType(elf, SHT_DYNSYM);
  } else {
    Die() << "unsupported ELF type: '"
          << ElfHeaderTypeToString(elf_header.e_type) << "'";
  }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, SymbolTableEntry::SymbolType type) {
  using SymbolType = SymbolTableEntry::SymbolType;
  switch (type) {
    case SymbolType::NOTYPE:
      return os << "notype";
    case SymbolType::OBJECT:
      return os << "object";
    case SymbolType::FUNCTION:
      return os << "function";
    case SymbolType::SECTION:
      return os << "section";
    case SymbolType::FILE:
      return os << "file";
    case SymbolType::COMMON:
      return os << "common";
    case SymbolType::TLS:
      return os << "TLS";
    case SymbolType::GNU_IFUNC:
      return os << "indirect function";
  }
}

std::ostream& operator<<(std::ostream& os,
                         const SymbolTableEntry::ValueType type) {
  using ValueType = SymbolTableEntry::ValueType;
  switch (type) {
    case ValueType::UNDEFINED:
      return os << "undefined";
    case ValueType::ABSOLUTE:
      return os << "absolute";
    case ValueType::COMMON:
      return os << "common";
    case ValueType::RELATIVE_TO_SECTION:
      return os << "relative";
  }
}

ElfLoader::ElfLoader(const std::string& path, bool verbose)
    : verbose_(verbose), fd_(-1), elf_(nullptr) {
  fd_ = open(path.c_str(), O_RDONLY);
  Check(fd_ >= 0) << "Could not open " << path;
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  elf_ = elf_begin(fd_, ELF_C_READ, nullptr);
  Check(elf_ != nullptr) << "ELF data not found in " << path;
}

ElfLoader::ElfLoader(char* data, size_t size, bool verbose)
    : verbose_(verbose), fd_(-1), elf_(nullptr) {
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  elf_ = elf_memory(data, size);
  Check(elf_ != nullptr) << "Cannot initialize libelf with provided memory";
}

ElfLoader::~ElfLoader() {
  if (elf_) {
    elf_end(elf_);
  }
  if (fd_ >= 0) {
    close(fd_);
  }
}

std::string_view ElfLoader::GetBtfRawData() const {
  Elf_Scn* btf_section = GetSectionByName(elf_, ".BTF");
  Check(btf_section != nullptr) << ".BTF section is invalid";
  Elf_Data* elf_data = elf_rawdata(btf_section, 0);
  Check(elf_data != nullptr) << ".BTF section data is invalid";
  const char* btf_start = static_cast<char*>(elf_data->d_buf);
  const size_t btf_size = elf_data->d_size;
  return std::string_view(btf_start, btf_size);
}

std::vector<SymbolTableEntry> ElfLoader::GetElfSymbols() const {
  Elf_Scn* symbol_table_section = GetSymbolTableSection(elf_, verbose_);
  Check(symbol_table_section != nullptr)
      << "failed to find symbol table section";

  const auto [symbol_table_header, symbol_table_data] =
      GetSectionInfo(symbol_table_section);
  const size_t number_of_symbols = GetNumberOfEntries(symbol_table_header);

  std::vector<SymbolTableEntry> result;
  result.reserve(number_of_symbols);

  for (size_t i = 0; i < number_of_symbols; ++i) {
    GElf_Sym symbol;
    Check(gelf_getsym(symbol_table_data, i, &symbol) != nullptr)
        << "symbol (i = " << i << ") was not found";

    const auto name =
        GetString(elf_, symbol_table_header.sh_link, symbol.st_name);
    result.push_back(SymbolTableEntry{
        .name = name,
        .value = symbol.st_value,
        .size = symbol.st_size,
        .symbol_type = ParseSymbolType(GELF_ST_TYPE(symbol.st_info)),
        .binding = ParseSymbolBinding(GELF_ST_BIND(symbol.st_info)),
        .visibility =
            ParseSymbolVisibility(GELF_ST_VISIBILITY(symbol.st_other)),
        .value_type = ParseSymbolValueType(symbol.st_shndx),
    });
  }

  return result;
}

}  // namespace elf
}  // namespace stg
