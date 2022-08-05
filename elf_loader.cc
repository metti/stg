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
#include <string>
#include <string_view>

#include "error.h"

namespace stg {
namespace elf {

ElfLoader::ElfLoader(const std::string& path)
    : path_(path), fd_(-1), elf_(nullptr) {
  fd_ = open(path.c_str(), O_RDONLY);
  Check(fd_ >= 0) << "Could not open " << path;
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  elf_ = elf_begin(fd_, ELF_C_READ, nullptr);
  Check(elf_ != nullptr) << "ELF data not found in " << path;
}

ElfLoader::ElfLoader(char* data, size_t size)
    : path_("(memory)"), fd_(-1), elf_(nullptr) {
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

std::string_view ElfLoader::GetBtfRawData() const {
  Elf_Scn* btf_section = GetSectionByName(".BTF");
  Check(btf_section != nullptr) << path_ << ": .BTF section is invalid";
  Elf_Data* elf_data = elf_rawdata(btf_section, 0);
  Check(elf_data != nullptr) << path_ << ": .BTF section data is invalid";
  const char* btf_start = static_cast<char*>(elf_data->d_buf);
  const size_t btf_size = elf_data->d_size;
  return std::string_view(btf_start, btf_size);
}

}  // namespace elf
}  // namespace stg
