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

#ifndef STG_ELF_READER_H_
#define STG_ELF_READER_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "elf_loader.h"
#include "graph.h"

namespace stg {
namespace elf {

Id Read(Graph& graph, const std::string& path, bool process_dwarf,
        bool verbose);
Id Read(Graph& graph, char* data, size_t size, bool process_dwarf,
        bool verbose);

// For unit tests only
namespace internal {

using SymbolTable = std::vector<SymbolTableEntry>;
using SymbolNameList = std::unordered_set<std::string_view>;
using CRCValuesMap = std::unordered_map<std::string, ElfSymbol::CRC>;
using NamespacesMap = std::unordered_map<std::string, std::string>;

ElfSymbol::SymbolType ConvertSymbolType(
    SymbolTableEntry::SymbolType symbol_type);
SymbolNameList GetKsymtabSymbols(const SymbolTable& symbols);
CRCValuesMap GetCRCValuesMap(const SymbolTable& symbols, const ElfLoader& elf);
NamespacesMap GetNamespacesMap(const SymbolTable& symbols,
                               const ElfLoader& elf);
bool IsPublicFunctionOrVariable(const SymbolTableEntry& symbol);

}  // namespace internal
}  // namespace elf
}  // namespace stg

#endif  // STG_ELF_READER_H_
