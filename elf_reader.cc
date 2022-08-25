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

#include "elf_reader.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "elf_loader.h"
#include "stg.h"

namespace stg {
namespace elf {

namespace {

using SymbolTable = std::vector<SymbolTableEntry>;

bool IsPublicFunctionOrVariable(const SymbolTableEntry& symbol) {
  const auto symbol_type = symbol.symbol_type;
  // Reject symbols that are not functions or variables.
  if (symbol_type != SymbolTableEntry::SymbolType::FUNCTION &&
      symbol_type != SymbolTableEntry::SymbolType::OBJECT &&
      symbol_type != SymbolTableEntry::SymbolType::TLS)
    return false;

  // Function or variable of ValueType::ABSOLUTE is not expected in any binary,
  // but GNU `ld` adds object of such type for every version name defined in
  // file. Such symbol should be rejected, because in fact it is not variable.
  if (symbol.value_type == SymbolTableEntry::ValueType::ABSOLUTE) {
    Check(symbol_type == SymbolTableEntry::SymbolType::OBJECT)
        << "Unexpected function or variable with ABSOLUTE value type";
    return false;
  }

  // Undefined symbol is dependency of the binary but is not part of ABI
  // provided by binary and should be rejected.
  if (symbol.value_type == SymbolTableEntry::ValueType::UNDEFINED)
    return false;

  // Local symbol is not visible outside the binary, so it is not public
  // and should be rejected.
  if (symbol.binding == SymbolTableEntry::Binding::LOCAL)
    return false;

  // "Hidden" and "internal" visibility values mean that symbol is not public
  // and should be rejected.
  if (symbol.visibility == SymbolTableEntry::Visibility::HIDDEN ||
      symbol.visibility == SymbolTableEntry::Visibility::INTERNAL)
    return false;

  return true;
}

Id Read(Graph& graph, elf::ElfLoader&& elf, bool verbose) {
  const auto all_symbols = elf.GetElfSymbols();
  if (verbose)
    std::cout << "Parsed " << all_symbols.size() << " symbols\n";

  std::vector<SymbolTableEntry> public_functions_and_variables;
  public_functions_and_variables.reserve(all_symbols.size());
  std::copy_if(all_symbols.begin(), all_symbols.end(),
               std::back_inserter(public_functions_and_variables),
               IsPublicFunctionOrVariable);
  public_functions_and_variables.shrink_to_fit();

  if (verbose)
    std::cout << "File has " << public_functions_and_variables.size()
              << " public functions and variables\n";

  std::map<SymbolKey, Id> symbols_map;
  return graph.Add(Make<Symbols>(symbols_map));
}

}  // namespace

Id Read(Graph& graph, const std::string& path, bool verbose) {
  if (verbose)
    std::cout << "Parsing ELF: " << path << '\n';

  return Read(graph, elf::ElfLoader(path, verbose), verbose);
}

Id Read(Graph& graph, char* data, size_t size, bool verbose) {
  if (verbose)
    std::cout << "Parsing ELF from memory\n";

  return Read(graph, elf::ElfLoader(data, size, verbose), verbose);
}

}  // namespace elf
}  // namespace stg
