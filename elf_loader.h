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

#ifndef STG_ELF_LOADER_H_
#define STG_ELF_LOADER_H_

#include <libelf.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "stg.h"

namespace stg {
namespace elf {

struct SymbolTableEntry {
  enum class SymbolType {
    NOTYPE = 0,
    OBJECT,
    FUNCTION,
    SECTION,
    FILE,
    COMMON,
    TLS,
    GNU_IFUNC
  };

  enum class ValueType {
    UNDEFINED = 0,
    ABSOLUTE,
    COMMON,
    RELATIVE_TO_SECTION,
  };

  using Binding = ElfSymbol::Binding;
  using Visibility = ElfSymbol::Visibility;

  std::string_view name;
  uint64_t value;
  uint64_t size;
  SymbolType symbol_type;
  Binding binding;
  Visibility visibility;
  ValueType value_type;
};

std::ostream& operator<<(std::ostream& os, SymbolTableEntry::SymbolType type);

std::ostream& operator<<(std::ostream& os,
                         const SymbolTableEntry::ValueType type);

class ElfLoader final {
 public:
  explicit ElfLoader(const std::string& path, bool verbose = false);
  ElfLoader(char* data, size_t size, bool verbose = false);
  ElfLoader(const ElfLoader&) = delete;
  ElfLoader& operator=(const ElfLoader&) = delete;
  ~ElfLoader();

  std::string_view GetBtfRawData() const;
  std::vector<SymbolTableEntry> GetElfSymbols() const;

 private:
  const bool verbose_;
  int fd_;
  Elf* elf_;
};

}  // namespace elf
}  // namespace stg

#endif  // STG_ELF_LOADER_H_
