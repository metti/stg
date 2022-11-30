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
// Author: Giuliano Procida

#ifndef STG_EQUALITY_H_
#define STG_EQUALITY_H_

#include <map>
#include <vector>
#include <utility>

#include "scc.h"
#include "stg.h"

namespace stg {

using Pair = std::pair<Id, Id>;

struct HashPair {
  size_t operator()(const Pair& comparison) const {
    size_t seed = 0;
    std::hash<Id> h;
    combine_hash(seed, h(comparison.first));
    combine_hash(seed, h(comparison.second));
    return seed;
  }
  static void combine_hash(size_t& seed, size_t hash) {
    seed ^= hash + 0x9e3779b97f4a7c15 + (seed << 12) + (seed >> 4);
  }
};

// TODO: Add UnionFind equality cache, if needed.

// Node equality algorithm. This only cares about node and edge attributes and
// is blind to node identity. It is generic over the equality cache which is fed
// information about equality results and queried for the same. Different
// implementations are possible depending on the needs of the caller and the
// invariants guaranteed.
template <typename EqualityCache>
struct Equals {
  Equals(const Graph& graph) : graph(graph) {}
  bool operator()(Id id1, Id id2);
  bool operator()(const std::vector<Id>& ids1, const std::vector<Id>& ids2);
  template <typename Key>
  bool operator()(const std::map<Key, Id>& ids1, const std::map<Key, Id>& ids2);
  bool operator()(const Void&, const Void&);
  bool operator()(const Variadic&, const Variadic&);
  bool operator()(const PointerReference&, const PointerReference&);
  bool operator()(const Typedef&, const Typedef&);
  bool operator()(const Qualified&, const Qualified&);
  bool operator()(const Primitive&, const Primitive&);
  bool operator()(const Array&, const Array&);
  bool operator()(const BaseClass&, const BaseClass&);
  bool operator()(const Member&, const Member&);
  bool operator()(const Method&, const Method&);
  bool operator()(const StructUnion&, const StructUnion&);
  bool operator()(const Enumeration&, const Enumeration&);
  bool operator()(const Function&, const Function&);
  bool operator()(const ElfSymbol&, const ElfSymbol&);
  bool operator()(const Symbols&, const Symbols&);
  bool Mismatch();
  const Graph& graph;
  EqualityCache equality_cache;
  SCC<Pair, HashPair> scc;
};

}  // namespace stg

#endif  // STG_EQUALITY_H_
