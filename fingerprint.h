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

#ifndef STG_FINGERPRINT_H_
#define STG_FINGERPRINT_H_

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "scc.h"
#include "stg.h"

namespace stg {

// Fingerprint is a node hasher that hashes all nodes reachable from a given
// root node. It will almost always succeed in distinguishing unequal nodes.
//
// Given any mutual dependencies between hashes, it falls back to a very poor
// but safe hash for the affected nodes: the size of the SCC.
class Fingerprint {
 public:
  Fingerprint(const Graph& graph, std::unordered_map<Id, uint64_t>& hashes)
      : graph(graph), hashes(hashes) {}

  void Process(Id root) {
    todo.insert(root);
    while (!todo.empty()) {
      for (auto id : std::exchange(todo, {})) {
        (*this)(id);
      }
    }
  }

  void DumpStats(std::ostream& os) const {
    for (const auto& [size, count] : non_trivial_scc_count) {
      os << "non_trivial_scc_count[" << size << "]=" << count << '\n';
    }
  }

  // Graph function implementation
  uint64_t operator()(const Void&) {
    return Hash('O');
  }

  uint64_t operator()(const Variadic&) {
    return Hash('V');
  }

  uint64_t operator()(const PointerReference& x) {
    return Hash(x.kind == PointerReference::Kind::POINTER ? 'P'
                : x.kind == PointerReference::Kind::LVALUE_REFERENCE ? 'L'
                : 'R',
                (*this)(x.pointee_type_id));
  }

  uint64_t operator()(const Typedef& x) {
    todo.insert(x.referred_type_id);
    return Hash('T', x.name);
  }

  uint64_t operator()(const Qualified& x) {
    return Hash(x.qualifier == Qualifier::CONST ? 'c'
                : x.qualifier == Qualifier::VOLATILE ? 'v'
                : 'r',
                (*this)(x.qualified_type_id));
  }

  uint64_t operator()(const Primitive& x) {
    return Hash('i', x.name);
  }

  uint64_t operator()(const Array& x) {
    return Hash('A', x.number_of_elements, (*this)(x.element_type_id));
  }

  uint64_t operator()(const BaseClass& x) {
    return Hash('B', (*this)(x.type_id));
  }

  uint64_t operator()(const Method& x) {
    return Hash('M', x.mangled_name, x.name, (*this)(x.type_id));
  }

  uint64_t operator()(const Member& x) {
    return Hash('D', x.name, x.offset, (*this)(x.type_id));
  }

  uint64_t operator()(const StructUnion& x) {
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

  uint64_t operator()(const Enumeration& x) {
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

  uint64_t operator()(const Function& x) {
    auto h = Hash('F', (*this)(x.return_type_id));
    for (const auto& parameter : x.parameters) {
      h = Hash(h, (*this)(parameter));
    }
    return h;
  }

  uint64_t operator()(const ElfSymbol& x) {
    if (x.type_id.has_value()) {
      todo.insert(x.type_id.value());
    }
    return Hash('S', x.symbol_name);
  }

  uint64_t operator()(const Symbols& x) {
    for (const auto& [name, symbol] : x.symbols) {
      todo.insert(symbol);
    }
    return Hash('Z');
  }

 private:
  uint64_t operator()(Id id) {
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

    uint64_t result = graph.Apply<uint64_t>(*this, id);

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
      ++non_trivial_scc_count[size];
    }
    for (auto id : ids) {
      hashes.insert({id, result});
    }
    return result;
  }

  uint64_t Hash(uint64_t x) const {
    return x;
  }

  uint64_t Hash(char x) const {
    return x;
  }

  uint64_t Hash(const std::string& x) const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (auto ch : x) {
      h ^= static_cast<unsigned char>(ch);
      h *= 0x00000100000001B3ull;
    }
    return h;
  }

  template <typename Arg, typename... Args>
  uint64_t Hash(uint64_t h, Arg arg, Args... args) const {
    return Hash(h ^ (Hash(arg) + 0x9e3779b9 + (h << 6) + (h >> 2)), args...);
  }

  const Graph& graph;
  std::unordered_map<Id, uint64_t>& hashes;
  std::unordered_set<Id> todo;
  SCC<Id> scc;

  std::map<size_t, size_t> non_trivial_scc_count;
};

}  // namespace stg

#endif  // STG_FINGERPRINT_H_
