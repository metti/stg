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
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "crc.h"
#include "elf_loader.h"
#include "stg.h"

namespace stg {
namespace elf {

namespace {

using SymbolTable = std::vector<SymbolTableEntry>;
using CRCValuesMap = std::unordered_map<std::string, CRC>;

ElfSymbol::SymbolType ConvertSymbolType(
    SymbolTableEntry::SymbolType symbol_type) {
  switch (symbol_type) {
    case SymbolTableEntry::SymbolType::OBJECT:
      return ElfSymbol::SymbolType::OBJECT;
    case SymbolTableEntry::SymbolType::FUNCTION:
      return ElfSymbol::SymbolType::FUNCTION;
    case SymbolTableEntry::SymbolType::COMMON:
      return ElfSymbol::SymbolType::COMMON;
    case SymbolTableEntry::SymbolType::TLS:
      return ElfSymbol::SymbolType::TLS;
    default:
      Die() << "Unsupported ELF symbol type: " << symbol_type;
  }
}

CRCValuesMap GetCRCValuesMap(const SymbolTable& symbols) {
  constexpr std::string_view kCRCPrefix = "__crc_";

  CRCValuesMap crc_values;

  for (const auto& symbol : symbols) {
    const std::string_view name = symbol.name;
    if (name.substr(0, kCRCPrefix.size()) == kCRCPrefix) {
      std::string_view name_suffix = name.substr(kCRCPrefix.size());
      bool emplaced = crc_values.emplace(name_suffix, CRC{symbol.value}).second;
      Check(emplaced) << "Multiple CRC values for symbol '" << name_suffix
                      << '\'';
    }
  }

  return crc_values;
}

template <typename M, typename K>
std::optional<typename M::mapped_type> MaybeGet(const M& map, const K& key) {
  const auto it = map.find(key);
  if (it == map.end()) {
    return {};
  }
  return {it->second};
}

ElfSymbol SymbolTableEntryToElfSymbol(const SymbolTableEntry& symbol,
                                      const CRCValuesMap& crc_values) {
  return ElfSymbol(
      /* symbol_name = */ std::string(symbol.name),
      /* version_info = */ std::nullopt,
      /* is_defined = */ symbol.value_type !=
          SymbolTableEntry::ValueType::UNDEFINED,
      /* symbol_type = */ ConvertSymbolType(symbol.symbol_type),
      /* binding = */ symbol.binding,
      /* visibility = */ symbol.visibility,
      /* crc = */ MaybeGet(crc_values, std::string(symbol.name)),
      /* ns = */ std::nullopt,        // TODO: Linux namespace
      /* type_id = */ std::nullopt,   // TODO: fill type ids
      /* full_name = */ std::nullopt  // TODO: fill full names
  );
}

bool IsPublicFunctionOrVariable(const SymbolTableEntry& symbol) {
  const auto symbol_type = symbol.symbol_type;
  // Reject symbols that are not functions or variables.
  if (symbol_type != SymbolTableEntry::SymbolType::FUNCTION &&
      symbol_type != SymbolTableEntry::SymbolType::OBJECT &&
      symbol_type != SymbolTableEntry::SymbolType::TLS) {
    return false;
  }

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
  if (symbol.value_type == SymbolTableEntry::ValueType::UNDEFINED) {
    return false;
  }

  // Local symbol is not visible outside the binary, so it is not public
  // and should be rejected.
  if (symbol.binding == SymbolTableEntry::Binding::LOCAL) {
    return false;
  }

  // "Hidden" and "internal" visibility values mean that symbol is not public
  // and should be rejected.
  if (symbol.visibility == SymbolTableEntry::Visibility::HIDDEN ||
      symbol.visibility == SymbolTableEntry::Visibility::INTERNAL) {
    return false;
  }

  return true;
}

Id Read(Graph& graph, elf::ElfLoader&& elf, bool verbose) {
  const auto all_symbols = elf.GetElfSymbols();
  if (verbose) {
    std::cout << "Parsed " << all_symbols.size() << " symbols\n";
  }

  std::vector<SymbolTableEntry> public_functions_and_variables;
  public_functions_and_variables.reserve(all_symbols.size());
  std::copy_if(all_symbols.begin(), all_symbols.end(),
               std::back_inserter(public_functions_and_variables),
               IsPublicFunctionOrVariable);
  public_functions_and_variables.shrink_to_fit();

  const CRCValuesMap crc_values =
      elf.IsLinuxKernelBinary() ? GetCRCValuesMap(all_symbols) : CRCValuesMap{};

  if (verbose) {
    std::cout << "File has " << public_functions_and_variables.size()
              << " public functions and variables:\n";
    for (const auto& symbol : public_functions_and_variables) {
      std::cout << "  " << symbol.binding << ' ' << symbol.symbol_type << " '"
                << symbol.name << "'\n    visibility=" << symbol.visibility
                << " size=" << symbol.size << " value=" << symbol.value << "["
                << symbol.value_type << "]\n";
    }
  }

  std::map<std::string, Id> symbols_map;
  for (const auto& symbol : public_functions_and_variables) {
    // TODO: add VersionInfoToString to SymbolKey name
    // TODO: check for uniqueness of SymbolKey in map after support
    // for version info
    symbols_map.emplace(std::string(symbol.name),
                        graph.Add(Make<ElfSymbol>(
                            SymbolTableEntryToElfSymbol(symbol, crc_values))));
  }
  return graph.Add(Make<Symbols>(std::move(symbols_map)));
}

}  // namespace

Id Read(Graph& graph, const std::string& path, bool verbose) {
  if (verbose) {
    std::cout << "Parsing ELF: " << path << '\n';
  }

  return Read(graph, elf::ElfLoader(path, verbose), verbose);
}

Id Read(Graph& graph, char* data, size_t size, bool verbose) {
  if (verbose) {
    std::cout << "Parsing ELF from memory\n";
  }

  return Read(graph, elf::ElfLoader(data, size, verbose), verbose);
}

}  // namespace elf
}  // namespace stg
