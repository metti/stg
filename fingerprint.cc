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

#include "fingerprint.h"

#include <string>
#include <unordered_set>

#include "hashing.h"
#include "scc.h"

namespace stg {
namespace {

struct Hasher {
  Hasher(const Graph& graph, std::unordered_map<Id, uint32_t>& hashes,
         std::unordered_set<Id>& todo, Metrics& metrics)
      : graph(graph), hashes(hashes), todo(todo),
        non_trivial_scc_size(metrics, "fingerprint.non_trivial_scc_size") {}

  // Graph function implementation
  uint32_t operator()(const Void&) {
    return hash('O');
  }

  uint32_t operator()(const Variadic&) {
    return hash('V');
  }

  uint32_t operator()(const PointerReference& x) {
    return hash('P', static_cast<uint32_t>(x.kind), (*this)(x.pointee_type_id));
  }

  uint32_t operator()(const Typedef& x) {
    todo.insert(x.referred_type_id);
    return hash('T', x.name);
  }

  uint32_t operator()(const Qualified& x) {
    return hash('Q', static_cast<uint32_t>(x.qualifier),
                (*this)(x.qualified_type_id));
  }

  uint32_t operator()(const Primitive& x) {
    return hash('i', x.name);
  }

  uint32_t operator()(const Array& x) {
    return hash('A', x.number_of_elements, (*this)(x.element_type_id));
  }

  uint32_t operator()(const BaseClass& x) {
    return hash('B', (*this)(x.type_id));
  }

  uint32_t operator()(const Method& x) {
    return hash('M', x.mangled_name, x.name, (*this)(x.type_id));
  }

  uint32_t operator()(const Member& x) {
    return hash('D', x.name, x.offset, (*this)(x.type_id));
  }

  uint32_t operator()(const StructUnion& x) {
    auto kind = hash('U', static_cast<uint32_t>(x.kind));
    if (x.name.empty()) {
      auto h = kind;
      if (x.definition.has_value()) {
        auto& definition = *x.definition;
        for (auto id : definition.base_classes) {
          todo.insert(id);
        }
        for (auto id : definition.methods) {
          todo.insert(id);
        }
        for (auto id : definition.members) {
          h = hash(h, (*this)(id));
        }
      }
      return h;
    } else {
      if (x.definition.has_value()) {
        auto& definition = *x.definition;
        for (auto id : definition.base_classes) {
          todo.insert(id);
        }
        for (auto id : definition.methods) {
          todo.insert(id);
        }
        for (auto id : definition.members) {
          todo.insert(id);
        }
      }
      return hash(kind, x.name, x.definition ? '1' : '0');
    }
  }

  uint32_t operator()(const Enumeration& x) {
    if (x.name.empty()) {
      auto h = hash('e');
      if (x.definition) {
        for (const auto& e : x.definition->enumerators) {
          h = hash(h, e.first);
        }
      }
      return h;
    } else {
      return hash('E', x.name, x.definition ? '1' : '0');
    }
  }

  uint32_t operator()(const Function& x) {
    auto h = hash('F', (*this)(x.return_type_id));
    for (const auto& parameter : x.parameters) {
      h = hash(h, (*this)(parameter));
    }
    return h;
  }

  uint32_t operator()(const ElfSymbol& x) {
    if (x.type_id.has_value()) {
      todo.insert(x.type_id.value());
    }
    return hash('S', x.symbol_name);
  }

  uint32_t operator()(const Symbols& x) {
    for (const auto& [name, symbol] : x.symbols) {
      todo.insert(symbol);
    }
    return hash('Z');
  }

  // main entry point
  uint32_t operator()(Id id) {
    // Check if the id already has a fingerprint.
    const auto it = hashes.find(id);
    if (it != hashes.end()) {
      return it->second;
    }

    // Record the id with Strongly-Connected Component finder.
    auto handle = scc.Open(id);
    if (!handle) {
      // Already open.
      //
      // Return a dummy fingerprint.
      return 0;
    }
    // Comparison opened, need to close it before returning.

    uint32_t result = graph.Apply<uint32_t>(*this, id);

    // Check for a complete Strongly-Connected Component.
    auto ids = scc.Close(*handle);
    if (ids.empty()) {
      // Note that result is tentative as the SCC is still open.
      return result;
    }

    // Closed SCC.
    //
    // Note that result is the combination of every fingerprint in the SCC via
    // the DFS spanning tree.
    //
    // All nodes in a non-trivial SCCs are given the same fingerprint, but
    // non-trivial SCCs should be extremely rare.
    const auto size = ids.size();
    if (size > 1) {
      result = size;
      non_trivial_scc_size.Add(size);
    }
    for (auto id : ids) {
      hashes.insert({id, result});
    }
    return result;
  }

  const Graph& graph;
  std::unordered_map<Id, uint32_t>& hashes;
  std::unordered_set<Id> &todo;
  Histogram non_trivial_scc_size;
  SCC<Id> scc;

  // Function object: (Args...) -> uint32_t
  Hash hash;
};

}  // namespace

std::unordered_map<Id, uint32_t> Fingerprint(
    const Graph& graph, Id root, Metrics& metrics) {
  Time x(metrics, "hash nodes");
  std::unordered_map<Id, uint32_t> hashes;
  std::unordered_set<Id> todo;
  Hasher hasher(graph, hashes, todo, metrics);
  todo.insert(root);
  while (!todo.empty()) {
    for (auto id : std::exchange(todo, {})) {
      hasher(id);
    }
  }
  return hashes;
}

}  // namespace stg
