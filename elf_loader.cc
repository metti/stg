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

#include <gelf.h>
#include <libelf.h>

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

std::string PrintElfHeaderType(unsigned char elf_header_type) {
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

}  // namespace

ElfLoader::ElfLoader(const std::string& path, bool verbose)
    : path_(path), verbose_(verbose), fd_(-1), elf_(nullptr) {
  fd_ = open(path.c_str(), O_RDONLY);
  Check(fd_ >= 0) << "Could not open " << path;
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  elf_ = elf_begin(fd_, ELF_C_READ, nullptr);
  Check(elf_ != nullptr) << "ELF data not found in " << path;
}

ElfLoader::ElfLoader(char* data, size_t size, bool verbose)
    : path_("(memory)"), verbose_(verbose), fd_(-1), elf_(nullptr) {
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  elf_ = elf_memory(data, size);
  Check(elf_ != nullptr) << "Cannot initialize libelf with provided memory";
}

ElfLoader::~ElfLoader() {
  if (elf_)
    elf_end(elf_);
  if (fd_ >= 0)
    close(fd_);
}

std::vector<Elf_Scn*> ElfLoader::GetSectionsIf(
    std::function<bool(const GElf_Shdr&)> predicate) const {
  std::vector<Elf_Scn*> result;
  Elf_Scn* section = nullptr;
  GElf_Shdr header;
  while ((section = elf_nextscn(elf_, section)) != nullptr) {
    Check(gelf_getshdr(section, &header) != nullptr)
        << path_ << ": could not get ELF section header";
    if (predicate(header))
      result.push_back(section);
  }
  return result;
}

std::vector<Elf_Scn*> ElfLoader::GetSectionsByName(
    const std::string& name) const {
  size_t shdr_strtab_index;
  Check(elf_getshdrstrndx(elf_, &shdr_strtab_index) == 0)
      << path_ << ": could not get ELF section header string table index";
  return GetSectionsIf([&](const GElf_Shdr& header) {
    return elf_strptr(elf_, shdr_strtab_index, header.sh_name) == name;
  });
}

Elf_Scn* ElfLoader::GetSectionByName(const std::string& name) const {
  const auto sections = GetSectionsByName(name);
  Check(!sections.empty())
      << path_ << ": no section found with name '" << name << "'";
  Check(sections.size() == 1)
      << path_ << ": multiple sections found with name '" << name << "'";
  return sections[0];
}

Elf_Scn* ElfLoader::GetSectionByType(Elf64_Word type) const {
  auto sections = GetSectionsIf([&](const GElf_Shdr& header) {
    return header.sh_type == type;
  });
  Check(!sections.empty())
      << path_ << ": no section found with type " << type;
  Check(sections.size() == 1)
      << path_ << ": multiple sections found with type " << type;
  return sections[0];
}

std::string_view ElfLoader::GetBtfRawData() const {
  Elf_Scn* btf_section = GetSectionByName(".BTF");
  Check(btf_section != nullptr) << path_ << ": .BTF section is invalid";
  Elf_Data* elf_data = elf_rawdata(btf_section, 0);
  Check(elf_data != nullptr) << path_ << ": .BTF section data is invalid";
  const char* btf_start = static_cast<char*>(elf_data->d_buf);
  const size_t btf_size = elf_data->d_size;
  return std::string_view(btf_start, btf_size);
}

Elf_Scn* ElfLoader::GetSymbolTableSection() const {
  GElf_Ehdr elf_header;
  Check(gelf_getehdr(elf_, &elf_header) != nullptr)
      << path_ << ": could not get ELF header";

  if (verbose_)
    std::cout << "ELF type: " << PrintElfHeaderType(elf_header.e_type) << '\n';
  // TODO: check if vmlinux symbol table type matches ELF type
  // same way as other binaries
  if (elf_header.e_type == ET_REL)
    return GetSectionByType(SHT_SYMTAB);
  else if (elf_header.e_type == ET_DYN || elf_header.e_type == ET_EXEC)
    return GetSectionByType(SHT_DYNSYM);
  else
    Die() << path_ << ": unsupported ELF type: '"
          << PrintElfHeaderType(elf_header.e_type) << "'";
}

std::string ElfLoader::GetSymbolName(const GElf_Shdr& symbol_table_header,
                                     const GElf_Sym& symbol) const {
  const auto name =
      elf_strptr(elf_, symbol_table_header.sh_link, symbol.st_name);

  Check(name != nullptr) << path_ << ": symbol name was not found (section: "
                         << symbol_table_header.sh_link
                         << ", name index: " << symbol.st_name << ")";
  return name;
}

std::vector<SymbolTableEntry> ElfLoader::GetElfSymbols() const {
  Elf_Scn* symbol_table_section = GetSymbolTableSection();
  Check(symbol_table_section != nullptr)
      << path_ << ": failed to find symbol table section";

  GElf_Shdr symbol_table_header;
  Check(gelf_getshdr(symbol_table_section, &symbol_table_header) != nullptr)
      << path_ << ": failed to read symbol table header";
  Check(symbol_table_header.sh_entsize != 0)
      << path_ << ": zero symbol table entity size is unexpected";
  Elf_Data* symbol_table_data = elf_getdata(symbol_table_section, 0);
  Check(symbol_table_data != nullptr)
      << path_ << ": symbol table data is invalid";

  const size_t number_of_symbols =
      symbol_table_header.sh_size / symbol_table_header.sh_entsize;

  std::vector<SymbolTableEntry> result;
  result.reserve(number_of_symbols);

  for (size_t i = 0; i < number_of_symbols; ++i) {
    GElf_Sym symbol;
    Check(gelf_getsym(symbol_table_data, i, &symbol) != nullptr)
        << path_ << ": symbol (i = " << i << ") was not found";

    const std::string name = GetSymbolName(symbol_table_header, symbol);
    result.push_back(SymbolTableEntry{
        .name = name,
        .value = symbol.st_value,
        .size = symbol.st_size,
        .symbol_type = ParseSymbolType(GELF_ST_TYPE(symbol.st_info)),
        .binding = ParseSymbolBinding(GELF_ST_BIND(symbol.st_info)),
        .visibility = ParseSymbolVisibility(GELF_ST_VISIBILITY(symbol.st_info)),
        .value_type = ParseSymbolValueType(symbol.st_shndx),
    });
  }

  return result;
}

}  // namespace elf
}  // namespace stg
