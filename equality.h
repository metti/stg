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

// Node equality algorithm. This only cares about node and edge attributes and
// is blind to node identity. It is generic over the equality cache which is fed
// information about equality results and queried for the same. Different
// implementations are possible depending on the needs of the caller and the
// guaranteed invariants.
template <typename EqualityCache>
struct Equals {
  Equals(const Graph& graph, EqualityCache& equality_cache)
      : graph(graph), equality_cache(equality_cache) {}

  bool operator()(Id id1, Id id2) {
    const Pair comparison = {id1, id2};

    // Check if the comparison has an already known result.
    const auto check = equality_cache.Query(comparison);
    if (check.has_value()) {
      return check.value();
    }

    // Record the comparison with Strongly-Connected Component finder.
    auto handle = scc.Open(comparison);
    if (!handle) {
      // Already open.
      //
      // Return a dummy true outcome.
      return true;
    }
    // Comparison opened, need to close it before returning.

    bool result = graph.Apply2<bool>(*this, id1, id2);

    // Check for a complete Strongly-Connected Component.
    auto comparisons = scc.Close(*handle);
    if (comparisons.empty()) {
      // Note that result is tentative as the SCC is still open.
      return result;
    }

    // Closed SCC.
    //
    // Note that result is the conjunction of every equality in the SCC via the
    // DFS spanning tree.
    if (result) {
      equality_cache.AllSame(comparisons);
    } else {
      equality_cache.AllDifferent(comparisons);
    }
    return result;
  }

  bool operator()(const std::vector<Id>& ids1, const std::vector<Id>& ids2) {
    bool result = ids1.size() == ids2.size();
    for (size_t ix = 0; result && ix < ids1.size(); ++ix) {
      result = (*this)(ids1[ix], ids2[ix]);
    }
    return result;
  }

  template <typename Key>
  bool operator()(const std::map<Key, Id>& ids1,
                  const std::map<Key, Id>& ids2) {
    bool result = ids1.size() == ids2.size();
    auto it1 = ids1.begin();
    auto it2 = ids2.begin();
    const auto end1 = ids1.end();
    const auto end2 = ids2.end();
    while (result && it1 != end1 && it2 != end2) {
      result = it1->first == it2->first
               && (*this)(it1->second, it2->second);
      ++it1;
      ++it2;
    }
    return result && it1 == end1 && it2 == end2;
  }

  bool operator()(const Void&, const Void&) {
    return true;
  }

  bool operator()(const Variadic&, const Variadic&) {
    return true;
  }

  bool operator()(const PointerReference& x1,
                  const PointerReference& x2) {
    return x1.kind == x2.kind
        && (*this)(x1.pointee_type_id, x2.pointee_type_id);
  }

  bool operator()(const Typedef& x1, const Typedef& x2) {
    return x1.name == x2.name
        && (*this)(x1.referred_type_id, x2.referred_type_id);
  }

  bool operator()(const Qualified& x1, const Qualified& x2) {
    return x1.qualifier == x2.qualifier
        && (*this)(x1.qualified_type_id, x2.qualified_type_id);
  }

  bool operator()(const Primitive& x1, const Primitive& x2) {
    return x1.name == x2.name
        && x1.encoding == x2.encoding
        && x1.bitsize == x2.bitsize
        && x1.bytesize == x2.bytesize;
  }

  bool operator()(const Array& x1, const Array& x2) {
    return x1.number_of_elements == x2.number_of_elements
        && (*this)(x1.element_type_id, x2.element_type_id);
  }

  bool operator()(const BaseClass& x1, const BaseClass& x2) {
    return x1.offset == x2.offset
        && x1.inheritance == x2.inheritance
        && (*this)(x1.type_id, x2.type_id);
  }

  bool operator()(const Method& x1, const Method& x2) {
    return x1.mangled_name == x2.mangled_name
        && x1.name == x2.name
        && x1.kind == x2.kind
        && x1.vtable_offset == x2.vtable_offset
        && (*this)(x1.type_id, x2.type_id);
  }

  bool operator()(const Member& x1, const Member& x2) {
    return x1.name == x2.name
        && x1.offset == x2.offset
        && x1.bitsize == x2.bitsize
        && (*this)(x1.type_id, x2.type_id);
  }

  bool operator()(const StructUnion& x1, const StructUnion& x2) {
    const auto& definition1 = x1.definition;
    const auto& definition2 = x2.definition;
    bool result = x1.kind == x2.kind
                  && x1.name == x2.name
                  && definition1.has_value() == definition2.has_value();
    if (result && definition1.has_value()) {
      result = definition1->bytesize == definition2->bytesize
               && (*this)(definition1->base_classes, definition2->base_classes)
               && (*this)(definition1->methods, definition2->methods)
               && (*this)(definition1->members, definition2->members);
    }
    return result;
  }

  bool operator()(const Enumeration& x1, const Enumeration& x2) {
    const auto& definition1 = x1.definition;
    const auto& definition2 = x2.definition;
    bool result = x1.name == x2.name
                  && definition1.has_value() == definition2.has_value();
    if (result && definition1.has_value()) {
      result = definition1->bytesize == definition2->bytesize
               && definition1->enumerators == definition2->enumerators;
    }
    return result;
  }

  bool operator()(const Function& x1, const Function& x2) {
    return (*this)(x1.parameters, x2.parameters)
        && (*this)(x1.return_type_id, x2.return_type_id);
  }

  bool operator()(const ElfSymbol& x1, const ElfSymbol& x2) {
    bool result = x1.symbol_name == x2.symbol_name
                  && x1.version_info == x2.version_info
                  && x1.is_defined == x2.is_defined
                  && x1.symbol_type == x2.symbol_type
                  && x1.binding == x2.binding
                  && x1.visibility == x2.visibility
                  && x1.crc == x2.crc
                  && x1.ns == x2.ns
                  && x1.full_name == x2.full_name
                  && x1.type_id.has_value() == x2.type_id.has_value();
    if (result && x1.type_id.has_value()) {
      result = (*this)(x1.type_id.value(), x2.type_id.value());
    }
    return result;
  }

  bool operator()(const Symbols& x1, const Symbols& x2) {
    return (*this)(x1.symbols, x2.symbols);
  }

  bool Mismatch() {
    return false;
  }

  const Graph& graph;
  EqualityCache& equality_cache;
  SCC<Pair, HashPair> scc;
};

}  // namespace stg

#endif  // STG_EQUALITY_H_
