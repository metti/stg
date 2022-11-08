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

#ifndef STG_DWARF_H_
#define STG_DWARF_H_

#include <elf.h>
#include <elfutils/libdw.h>

#include <cstddef>
#include <memory>
#include <string>

#include "file_descriptor.h"

namespace stg {
namespace dwarf {

// C++ wrapper over libdw (DWARF library).
//
// Creates a "Dwarf" object from an ELF file or a memory and controls the life
// cycle of the created objects.
class Handler {
 public:
  explicit Handler(const std::string& path);
  Handler(char* data, size_t size);

 private:
  struct ElfDeleter {
    void operator()(Elf* elf) { elf_end(elf); }
  };

  struct DwarfDeleter {
    void operator()(Dwarf* dwarf) { dwarf_end(dwarf); }
  };

  void InitialiseDwarf();

  // The order of the following three fields is important because Elf uses a
  // value from FileDescriptor without owning it, and Dwarf uses an Elf*.
  FileDescriptor fd_;
  std::unique_ptr<Elf, ElfDeleter> elf_;
  std::unique_ptr<Dwarf, DwarfDeleter> dwarf_;
};

}  // namespace dwarf
}  // namespace stg

#endif  // STG_DWARF_H_
