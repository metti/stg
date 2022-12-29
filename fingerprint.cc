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
    return Hash('O');
  }

  uint32_t operator()(const Variadic&) {
    return Hash('V');
  }

  uint32_t operator()(const PointerReference& x) {
    return Hash(x.kind == PointerReference::Kind::POINTER ? 'P'
                : x.kind == PointerReference::Kind::LVALUE_REFERENCE ? 'L'
                : 'R',
                (*this)(x.pointee_type_id));
  }

  uint32_t operator()(const Typedef& x) {
    todo.insert(x.referred_type_id);
    return Hash('T', x.name);
  }

  uint32_t operator()(const Qualified& x) {
    return Hash(x.qualifier == Qualifier::CONST ? 'c'
                : x.qualifier == Qualifier::VOLATILE ? 'v'
                : 'r',
                (*this)(x.qualified_type_id));
  }

  uint32_t operator()(const Primitive& x) {
    return Hash('i', x.name);
  }

  uint32_t operator()(const Array& x) {
    return Hash('A', x.number_of_elements, (*this)(x.element_type_id));
  }

  uint32_t operator()(const BaseClass& x) {
    return Hash('B', (*this)(x.type_id));
  }

  uint32_t operator()(const Method& x) {
    return Hash('M', x.mangled_name, x.name, (*this)(x.type_id));
  }

  uint32_t operator()(const Member& x) {
    return Hash('D', x.name, x.offset, (*this)(x.type_id));
  }

  uint32_t operator()(const StructUnion& x) {
    auto kind = Hash(x.kind == StructUnion::Kind::STRUCT ? 's'
                     : x.kind == StructUnion::Kind::UNION ? 'u'
                     : 'k');
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
          h = Hash(h, (*this)(id));
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
      return Hash(kind, x.name, x.definition ? '1' : '0');
    }
  }

  uint32_t operator()(const Enumeration& x) {
    if (x.name.empty()) {
      auto h = Hash('e');
      if (x.definition) {
        for (const auto& e : x.definition->enumerators) {
          h = Hash(h, e.first);
        }
      }
      return h;
    } else {
      return Hash('E', x.name, x.definition ? '1' : '0');
    }
  }

  uint32_t operator()(const Function& x) {
    auto h = Hash('F', (*this)(x.return_type_id));
    for (const auto& parameter : x.parameters) {
      h = Hash(h, (*this)(parameter));
    }
    return h;
  }

  uint32_t operator()(const ElfSymbol& x) {
    if (x.type_id.has_value()) {
      todo.insert(x.type_id.value());
    }
    return Hash('S', x.symbol_name);
  }

  uint32_t operator()(const Symbols& x) {
    for (const auto& [name, symbol] : x.symbols) {
      todo.insert(symbol);
    }
    return Hash('Z');
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

  // hash 64 bits by splitting, hashing and combining
  uint32_t Hash(uint64_t x) const {
    uint32_t lo = x;
    uint32_t hi = x >> 32;
    return Hash(lo, hi);
  }

  uint32_t Hash(uint32_t x) const {
    return x;
  }

  uint32_t Hash(char x) const {
    return x;
  }

  // 32-bit FNV-1a
  uint32_t Hash(const std::string& x) const {
    uint32_t h = 0x811c9dc5;
    for (auto ch : x) {
      h ^= static_cast<unsigned char>(ch);
      h *= 0x01000193;
    }
    return h;
  }

  template <typename Arg, typename... Args>
  uint32_t Hash(uint32_t h, Arg arg, Args... args) const {
    return Hash(h ^ (Hash(arg) + 0x9e3779b9 + (h << 6) + (h >> 2)), args...);
  }

  const Graph& graph;
  std::unordered_map<Id, uint32_t>& hashes;
  std::unordered_set<Id> &todo;
  Histogram non_trivial_scc_size;
  SCC<Id> scc;
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
