// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2022 Google LLC
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
// Author: Aleksei Vetrov

#include "dwarf.h"

#include <elf.h>
#include <elfutils/libdw.h>
#include <fcntl.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "error.h"

namespace stg {
namespace dwarf {

namespace {

constexpr int kReturnOk = 0;
constexpr int kReturnNoEntry = 1;

}  // namespace

Handler::Handler(const std::string& path)
    : fd_(path.c_str(), O_RDONLY) {
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  elf_ = std::unique_ptr<Elf, ElfDeleter>(
      elf_begin(fd_.Value(), ELF_C_READ, nullptr));
  InitialiseDwarf();
}

Handler::Handler(char* data, size_t size) {
  Check(elf_version(EV_CURRENT) != EV_NONE) << "ELF version mismatch";
  elf_ = std::unique_ptr<Elf, ElfDeleter>(elf_memory(data, size));
  InitialiseDwarf();
}

void Handler::InitialiseDwarf() {
  Check(elf_.get()) << "Couldn't open ELF: " << elf_errmsg(-1);
  dwarf_ = std::unique_ptr<::Dwarf, DwarfDeleter>(
      dwarf_begin_elf(elf_.get(), DWARF_C_READ, nullptr));
  Check(dwarf_.get()) << "dwarf_begin_elf returned error: " << dwarf_errmsg(-1);
}

std::vector<Entry> Handler::GetCompilationUnits() {
  std::vector<Entry> result;
  Dwarf_Off offset = 0;
  while (true) {
    Dwarf_Off next_offset;
    size_t header_size = 0;
    int return_code =
        dwarf_next_unit(dwarf_.get(), offset, &next_offset, &header_size,
                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    Check(return_code == kReturnOk || return_code == kReturnNoEntry)
        << "dwarf_next_unit returned error";
    if (return_code == kReturnNoEntry) {
      break;
    }
    result.push_back({});
    Check(dwarf_offdie(dwarf_.get(), offset + header_size, &result.back().die))
        << "dwarf_offdie returned error";

    offset = next_offset;
  }
  return result;
}

std::vector<Entry> Entry::GetChildren() {
  Entry child;
  int return_code = dwarf_child(&die, &child.die);
  Check(return_code == kReturnOk || return_code == kReturnNoEntry)
      << "dwarf_child returned error";
  std::vector<Entry> result;
  while (return_code == kReturnOk) {
    result.push_back(child);
    return_code = dwarf_siblingof(&child.die, &child.die);
    Check(return_code == kReturnOk || return_code == kReturnNoEntry)
        << "dwarf_siblingof returned error";
  }
  return result;
}

int Entry::GetTag() {
  return dwarf_tag(&die);
}

Dwarf_Off Entry::GetOffset() {
  return dwarf_dieoffset(&die);
}

}  // namespace dwarf
}  // namespace stg
