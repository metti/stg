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

#include <cstddef>
#include <functional>
#include <iomanip>
#include <ios>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dwarf_processor.h"
#include "dwarf_wrappers.h"
#include "elf_loader.h"
#include "equality.h"
#include "equality_cache.h"
#include "error.h"
#include "graph.h"
#include "metrics.h"
#include "reader_options.h"
#include "type_normalisation.h"
#include "type_resolution.h"
#include "unification.h"

namespace stg {
namespace elf {
namespace internal {

namespace {

struct IsTypeDefined {
  bool operator()(const Typedef&) const {
    return true;
  }

  bool operator()(const StructUnion& x) const {
    return x.definition.has_value();
  }

  bool operator()(const Enumeration& x) const {
    return x.definition.has_value();
  }

  template <typename Node>
  bool operator()(const Node&) const {
    Die() << "expected a Typedef/StructUnion/Enumeration node";
  }
};

template <typename M, typename K>
std::optional<typename M::mapped_type> MaybeGet(const M& map, const K& key) {
  const auto it = map.find(key);
  if (it == map.end()) {
    return {};
  }
  return {it->second};
}

}  // namespace

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
    case SymbolTableEntry::SymbolType::GNU_IFUNC:
      return ElfSymbol::SymbolType::GNU_IFUNC;
    default:
      Die() << "Unsupported ELF symbol type: " << symbol_type;
  }
}

SymbolNameList GetKsymtabSymbols(const SymbolTable& symbols) {
  constexpr std::string_view kKsymtabPrefix = "__ksymtab_";
  SymbolNameList result;
  result.reserve(symbols.size() / 2);
  for (const auto& symbol : symbols) {
    if (symbol.name.substr(0, kKsymtabPrefix.size()) == kKsymtabPrefix) {
      result.emplace(symbol.name.substr(kKsymtabPrefix.size()));
    }
  }
  return result;
}

CRCValuesMap GetCRCValuesMap(const SymbolTable& symbols, const ElfLoader& elf) {
  constexpr std::string_view kCRCPrefix = "__crc_";

  CRCValuesMap crc_values;

  for (const auto& symbol : symbols) {
    const std::string_view name = symbol.name;
    if (name.substr(0, kCRCPrefix.size()) == kCRCPrefix) {
      std::string_view name_suffix = name.substr(kCRCPrefix.size());
      if (!crc_values.emplace(name_suffix, elf.GetElfSymbolCRC(symbol))
               .second) {
        Die() << "Multiple CRC values for symbol '" << name_suffix << '\'';
      }
    }
  }

  return crc_values;
}

NamespacesMap GetNamespacesMap(const SymbolTable& symbols,
                               const ElfLoader& elf) {
  constexpr std::string_view kNSPrefix = "__kstrtabns_";

  NamespacesMap namespaces;

  for (const auto& symbol : symbols) {
    const std::string_view name = symbol.name;
    if (name.substr(0, kNSPrefix.size()) == kNSPrefix) {
      const std::string_view name_suffix = name.substr(kNSPrefix.size());
      const std::string_view ns = elf.GetElfSymbolNamespace(symbol);
      if (ns.empty()) {
        // The global namespace is explicitly represented as the empty string,
        // but the common interpretation is that such symbols lack an export
        // namespace.
        continue;
      }
      if (!namespaces.emplace(name_suffix, ns).second) {
        Die() << "Multiple namespaces for symbol '" << name_suffix << '\'';
      }
    }
  }

  return namespaces;
}

bool IsPublicFunctionOrVariable(const SymbolTableEntry& symbol) {
  const auto symbol_type = symbol.symbol_type;
  // Reject symbols that are not functions or variables.
  if (symbol_type != SymbolTableEntry::SymbolType::FUNCTION &&
      symbol_type != SymbolTableEntry::SymbolType::OBJECT &&
      symbol_type != SymbolTableEntry::SymbolType::TLS &&
      symbol_type != SymbolTableEntry::SymbolType::GNU_IFUNC) {
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

namespace {

class Reader {
 public:
  Reader(Graph& graph, const std::string& path, ReadOptions options,
         Metrics& metrics)
      : graph_(graph),
        dwarf_(path),
        elf_(dwarf_.GetElf(), options.Test(ReadOptions::INFO)),
        options_(options),
        equality_cache_(metrics),
        equals_(graph, equality_cache_),
        metrics_(metrics) {}

  Reader(Graph& graph, char* data, size_t size, ReadOptions options,
         Metrics& metrics)
      : graph_(graph),
        dwarf_(data, size),
        elf_(dwarf_.GetElf(), options.Test(ReadOptions::INFO)),
        options_(options),
        equality_cache_(metrics),
        equals_(graph, equality_cache_),
        metrics_(metrics) {}

  Id Read();
  ElfSymbol SymbolTableEntryToElfSymbol(const SymbolTableEntry& symbol) const;

 private:
  void GetTypesFromDwarf(dwarf::Handler& dwarf, bool is_little_endian_binary) {
    types_ = dwarf::Process(dwarf, is_little_endian_binary, graph_);

    // resolve types
    std::vector<Id> roots;
    roots.reserve(types_.named_type_ids.size() + types_.symbols.size());
    for (const auto& symbol : types_.symbols) {
      roots.push_back(symbol.id);
    }
    for (const auto id : types_.named_type_ids) {
      roots.push_back(id);
    }
    Unification unification(graph_, metrics_);
    stg::ResolveTypes(graph_, unification, roots, metrics_);
    unification.Substitute(graph_, metrics_);
    for (auto& id : types_.named_type_ids) {
      unification.Update(id);
    }
    for (auto& symbol : types_.symbols) {
      unification.Update(symbol.id);
    }

    // fill address to id
    //
    // In general, we want to handle as many of the following cases as possible.
    // In practice, determining the correct ELF-DWARF match may be impossible.
    //
    // * compiler-driven aliasing - multiple symbols with same address
    // * zero-size symbol false aliasing - multiple symbols and types with same
    //   address
    // * weak/strong linkage symbols - multiple symbols and types with same
    //   address
    // * assembly symbols - multiple declarations but no definition and no
    //   address in DWARF.
    for (size_t i = 0; i < types_.symbols.size(); ++i) {
      const auto& symbol = types_.symbols[i];

      // TODO: support linkage_name to support C++
      auto [it, emplaced] = address_name_to_index_.emplace(
          std::make_pair(symbol.address, symbol.name), i);
      if (!emplaced) {
        const auto& other = types_.symbols[it->second];
        // TODO: allow "compatible" duplicates, for example
        // "void foo(int bar)" vs "void foo(const int bar)"
        if (!IsEqual(symbol, other)) {
          Die() << "Duplicate DWARF symbol: address=" << Hex(symbol.address)
                << ", name=" << symbol.name;
        }
      }
    }
  }

  bool IsEqual(const dwarf::Types::Symbol& lhs,
               const dwarf::Types::Symbol& rhs) {
    return lhs.name == rhs.name && lhs.linkage_name == rhs.linkage_name
        && lhs.address == rhs.address && equals_(lhs.id, rhs.id);
  }

  void MaybeAddTypeInfo(const size_t address, ElfSymbol& node) const {
    // try to find the first symbol with given address
    const auto start_it = address_name_to_index_.lower_bound(
        std::make_pair(address, std::string()));
    const dwarf::Types::Symbol* best_symbol = nullptr;
    bool matched_by_name = false;
    size_t candidates = 0;
    for (auto it = start_it;
         it != address_name_to_index_.end() && it->first.first == address;
         ++it) {
      ++candidates;
      // We have at least matching addresses.
      const auto& candidate = types_.symbols[it->second];
      if (it->first.second == node.symbol_name) {
        // If we have also matching names we can stop looking further.
        matched_by_name = true;
        best_symbol = &candidate;
        break;
      }
      if (best_symbol == nullptr) {
        // Otherwise keep the first match.
        best_symbol = &candidate;
      }
    }
    if (best_symbol != nullptr) {
      // There may be multiple DWARF symbols with same address (zero-length
      // arrays), or ELF symbol has different name from DWARF symbol (aliases).
      // But if we have both situations at once, we can't match ELF to DWARF and
      // it should be fixed in analysed binary source code.
      Check(matched_by_name || candidates == 1)
          << "multiple candidates without matching names, best_symbol.name="
          << best_symbol->name;
      node.type_id = best_symbol->id;
      node.full_name = best_symbol->name;
    }
  }

  Graph& graph_;
  // The order of the following two fields is important because ElfLoader uses
  // an Elf* from dwarf::Handler without owning it.
  dwarf::Handler dwarf_;
  elf::ElfLoader elf_;
  ReadOptions options_;

  // Data extracted from ELF
  CRCValuesMap crc_values_;
  NamespacesMap namespaces_;
  // Data extracted from DWARF
  dwarf::Types types_;
  std::map<std::pair<size_t, std::string>, size_t> address_name_to_index_;

  // For checking type equality
  SimpleEqualityCache equality_cache_;
  Equals<SimpleEqualityCache> equals_;

  Metrics& metrics_;
};

Id Reader::Read() {
  const auto all_symbols = elf_.GetElfSymbols();
  if (options_.Test(ReadOptions::INFO)) {
    std::cout << "Parsed " << all_symbols.size() << " symbols\n";
  }

  const bool is_linux_kernel = elf_.IsLinuxKernelBinary();
  const SymbolNameList ksymtab_symbols =
      is_linux_kernel ? GetKsymtabSymbols(all_symbols) : SymbolNameList();

  std::vector<SymbolTableEntry> public_functions_and_variables;
  public_functions_and_variables.reserve(all_symbols.size());
  for (const auto& symbol : all_symbols) {
    if (IsPublicFunctionOrVariable(symbol) &&
        (!is_linux_kernel || ksymtab_symbols.count(symbol.name))) {
      public_functions_and_variables.push_back(symbol);
    }
  }
  public_functions_and_variables.shrink_to_fit();

  if (is_linux_kernel) {
    crc_values_ = GetCRCValuesMap(all_symbols, elf_);
    namespaces_ = GetNamespacesMap(all_symbols, elf_);
  }

  if (options_.Test(ReadOptions::INFO)) {
    std::cout << "File has " << public_functions_and_variables.size()
              << " public functions and variables:\n";
    for (const auto& symbol : public_functions_and_variables) {
      std::cout << "  " << symbol.binding << ' ' << symbol.symbol_type << " '"
                << symbol.name << "'\n    visibility=" << symbol.visibility
                << " size=" << symbol.size << " value=" << symbol.value << "["
                << symbol.value_type << "]\n";
    }
  }

  std::map<std::string, Id> types_map;
  if (!options_.Test(ReadOptions::SKIP_DWARF)) {
    GetTypesFromDwarf(dwarf_, elf_.IsLittleEndianBinary());
    if (options_.Test(ReadOptions::TYPE_ROOTS)) {
      const IsTypeDefined is_type_defined;
      const InterfaceKey get_key(graph_);
      for (const auto id : types_.named_type_ids) {
        if (graph_.Apply<bool>(is_type_defined, id)) {
          const auto [it, inserted] = types_map.emplace(get_key(id), id);
          if (!inserted) {
            Die() << "found conflicting interface type: " << it->first;
          }
        }
      }
    }
  }

  std::map<std::string, Id> symbols_map;
  for (const auto& symbol : public_functions_and_variables) {
    // TODO: add VersionInfoToString to SymbolKey name
    // TODO: check for uniqueness of SymbolKey in map after support
    // for version info
    symbols_map.emplace(
        std::string(symbol.name),
        graph_.Add<ElfSymbol>(SymbolTableEntryToElfSymbol(symbol)));
  }
  auto root =
      graph_.Add<Interface>(std::move(symbols_map), std::move(types_map));
  // Types produced by ELF/DWARF readers may require removing useless
  // qualifiers.
  RemoveUselessQualifiers(graph_, root);
  return root;
}

ElfSymbol Reader::SymbolTableEntryToElfSymbol(
    const SymbolTableEntry& symbol) const {
  ElfSymbol result(
      /* symbol_name = */ std::string(symbol.name),
      /* version_info = */ std::nullopt,
      /* is_defined = */ symbol.value_type !=
          SymbolTableEntry::ValueType::UNDEFINED,
      /* symbol_type = */ ConvertSymbolType(symbol.symbol_type),
      /* binding = */ symbol.binding,
      /* visibility = */ symbol.visibility,
      /* crc = */ MaybeGet(crc_values_, std::string(symbol.name)),
      /* ns = */ MaybeGet(namespaces_, std::string(symbol.name)),
      /* type_id = */ std::nullopt,
      /* full_name = */ std::nullopt);
  MaybeAddTypeInfo(elf_.GetAbsoluteAddress(symbol), result);
  return result;
}

}  // namespace
}  // namespace internal

Id Read(Graph& graph, const std::string& path, ReadOptions options,
        Metrics& metrics) {
  return internal::Reader(graph, path, options, metrics).Read();
}

Id Read(Graph& graph, char* data, size_t size, ReadOptions options,
        Metrics& metrics) {
  return internal::Reader(graph, data, size, options, metrics).Read();
}

}  // namespace elf
}  // namespace stg
