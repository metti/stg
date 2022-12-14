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

#ifndef STG_DWARF_WRAPPERS_H_
#define STG_DWARF_WRAPPERS_H_

#include <elf.h>
#include <elfutils/libdw.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "file_descriptor.h"

namespace stg {
namespace dwarf {

// C++ wrapper over Dwarf_Die, providing interface for its various properties.
struct Entry {
  // All methods in libdw take Dwarf_Die by non-const pointer as libdw caches
  // in it a link to the associated abbreviation table. Updating this link is
  // not thread-safe and so we cannot, for example, hold a std::shared_ptr to a
  // heap-allocated Dwarf_Die.
  //
  // The only options left are holding a std::unique_ptr or storing a value.
  // Unique pointers will add one more level of indirection to a hot path.
  // So we choose to store Dwarf_Die values.
  //
  // Each Entry only contains references to DWARF file memory and is fairly
  // small (32 bytes), so copies can be easily made if necessary. However,
  // within one thread it is preferable to pass it by reference.
  Dwarf_Die die{};

  // Get list of direct descendants of an entry in the DWARF tree.
  std::vector<Entry> GetChildren();

  // All getters are non-const as libdw may need to modify Dwarf_Die.
  int GetTag();
  Dwarf_Off GetOffset();
  std::optional<std::string> MaybeGetString(uint32_t attribute);
  std::optional<uint64_t> MaybeGetUnsignedConstant(uint32_t attribute);
};

// C++ wrapper over libdw (DWARF library).
//
// Creates a "Dwarf" object from an ELF file or a memory and controls the life
// cycle of the created objects.
class Handler {
 public:
  explicit Handler(const std::string& path);
  Handler(char* data, size_t size);

  std::vector<Entry> GetCompilationUnits();

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

#endif  // STG_DWARF_WRAPPERS_H_
