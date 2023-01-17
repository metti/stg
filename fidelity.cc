// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2023 Google LLC
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
// Author: Siddharth Nayyar

#include "fidelity.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graph.h"
#include "naming.h"

namespace stg {

namespace {

struct Fidelity {
  Fidelity(const Graph& graph, NameCache& name_cache)
      : graph(graph), describe(graph, name_cache) {}

  void operator()(Id);
  void operator()(const std::vector<Id>&);
  void operator()(const Void&, Id);
  void operator()(const Variadic&, Id);
  void operator()(const PointerReference&, Id);
  void operator()(const Typedef&, Id);
  void operator()(const Qualified&, Id);
  void operator()(const Primitive&, Id);
  void operator()(const Array&, Id);
  void operator()(const BaseClass&, Id);
  void operator()(const Method&, Id);
  void operator()(const Member&, Id);
  void operator()(const StructUnion&, Id);
  void operator()(const Enumeration&, Id);
  void operator()(const Function&, Id);
  void operator()(const ElfSymbol&, Id);
  void operator()(const Symbols&, Id);

  const Graph& graph;
  Describe describe;
  std::unordered_set<Id> seen;
  std::unordered_map<std::string, SymbolFidelity> symbols;
  std::unordered_map<std::string, TypeFidelity> types;
};

void Fidelity::operator()(Id id) {
  if (seen.insert(id).second) {
    graph.Apply<void>(*this, id, id);
  }
}

void Fidelity::operator()(const std::vector<Id>& x) {
  for (auto id : x) {
    (*this)(id);
  }
}

void Fidelity::operator()(const Void&, Id) {}

void Fidelity::operator()(const Variadic&, Id) {}

void Fidelity::operator()(const PointerReference& x, Id) {
  (*this)(x.pointee_type_id);
}

void Fidelity::operator()(const Typedef& x, Id) {
  (*this)(x.referred_type_id);
}

void Fidelity::operator()(const Qualified& x, Id) {
  (*this)(x.qualified_type_id);
}

void Fidelity::operator()(const Primitive&, Id) {}

void Fidelity::operator()(const Array& x, Id) {
  (*this)(x.element_type_id);
}

void Fidelity::operator()(const BaseClass& x, Id) {
  (*this)(x.type_id);
}

void Fidelity::operator()(const Method& x, Id) {
  (*this)(x.type_id);
}

void Fidelity::operator()(const Member& x, Id) {
  (*this)(x.type_id);
}

void Fidelity::operator()(const StructUnion& x, Id id) {
  auto [it, _] =
      types.emplace(describe(id).ToString(), TypeFidelity::DECLARATION_ONLY);
  if (x.definition) {
    it->second = TypeFidelity::FULLY_DEFINED;
    (*this)(x.definition->base_classes);
    (*this)(x.definition->methods);
    (*this)(x.definition->members);
  }
}

void Fidelity::operator()(const Enumeration& x, Id id) {
  auto [it, _] =
      types.emplace(describe(id).ToString(), TypeFidelity::DECLARATION_ONLY);
  if (x.definition) {
    it->second = TypeFidelity::FULLY_DEFINED;
  }
}

void Fidelity::operator()(const Function& x, Id) {
  (*this)(x.return_type_id);
  (*this)(x.parameters);
}

void Fidelity::operator()(const ElfSymbol& x, Id) {
  auto symbol = VersionedSymbolName(x);
  auto [it, _] = symbols.emplace(symbol, SymbolFidelity::UNTYPED);
  if (x.type_id) {
    it->second = SymbolFidelity::TYPED;
    (*this)(*x.type_id);
  }
}

void Fidelity::operator()(const Symbols& x, Id) {
  for (const auto& [_, id] : x.symbols) {
    (*this)(id);
  }
}

}  // namespace

}  // namespace stg
