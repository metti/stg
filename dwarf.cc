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

#include "error.h"

namespace stg {
namespace dwarf {

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

int Entry::GetTag() {
  return dwarf_tag(&die);
}

}  // namespace dwarf
}  // namespace stg
