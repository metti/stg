// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2022-2023 Google LLC
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

#include "type_resolution.h"

#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "error.h"
#include "substitution.h"

namespace stg {

namespace {

// Collect named type definition and declaration nodes.
struct NamedTypes {
  NamedTypes(const Graph& graph, Metrics& metrics)
      : graph(graph),
        nodes(metrics, "named_types.nodes"),
        types(metrics, "named_types.types"),
        definitions(metrics, "named_types.definitions"),
        declarations(metrics, "named_types.declarations") {}

  enum class Tag { STRUCT, UNION, ENUM };
  using Type = std::pair<Tag, std::string>;
  struct Info {
    std::vector<Id> definitions;
    std::vector<Id> declarations;
  };

  void operator()(const std::vector<Id>& ids) {
    for (auto id : ids) {
      (*this)(id);
    }
  }

  // main entry point
  void operator()(Id id) {
    if (seen.insert(id).second) {
      ++nodes;
      graph.Apply<void>(*this, id, id);
    }
  }

  Info& GetInfo(Tag tag, const std::string& name) {
    auto [it, inserted] = type_info.insert({{tag, name}, {}});
    if (inserted) {
      ++types;
    }
    return it->second;
  }

  // Graph function implementation
  void operator()(const Void&, Id) {}

  void operator()(const Variadic&, Id) {}

  void operator()(const PointerReference& x, Id) {
    (*this)(x.pointee_type_id);
  }

  void operator()(const Typedef& x, Id) {
    (*this)(x.referred_type_id);
  }

  void operator()(const Qualified& x, Id) {
    (*this)(x.qualified_type_id);
  }

  void operator()(const Primitive&, Id) {}

  void operator()(const Array& x, Id) {
    (*this)(x.element_type_id);
  }

  void operator()(const BaseClass& x, Id) {
    (*this)(x.type_id);
  }

  void operator()(const Method& x, Id) {
    (*this)(x.type_id);
  }

  void operator()(const Member& x, Id) {
    (*this)(x.type_id);
  }

  void operator()(const StructUnion& x, Id id) {
    auto tag = x.kind == StructUnion::Kind::STRUCT ? Tag::STRUCT : Tag::UNION;
    const auto& name = x.name;
    const bool named = !name.empty();
    auto& info = GetInfo(tag, name);
    if (x.definition.has_value()) {
      if (named) {
        info.definitions.push_back(id);
        ++definitions;
      }
      const auto& definition = *x.definition;
      (*this)(definition.base_classes);
      (*this)(definition.methods);
      (*this)(definition.members);
    } else {
      Check(named) << "anonymous forward declaration";
      info.declarations.push_back(id);
      incomplete.insert(id);
      ++declarations;
    }
  }

  void operator()(const Enumeration& x, Id id) {
    const auto& name = x.name;
    const bool named = !name.empty();
    auto& info = GetInfo(Tag::ENUM, name);
    if (x.definition) {
      if (named) {
        info.definitions.push_back(id);
        ++definitions;
      }
    } else {
      Check(named) << "anonymous forward declaration";
      info.declarations.push_back(id);
      incomplete.insert(id);
      ++declarations;
    }
  }

  void operator()(const Function& x, Id) {
    (*this)(x.return_type_id);
    (*this)(x.parameters);
  }

  void operator()(const ElfSymbol& x, Id) {
    if (x.type_id.has_value()) {
      (*this)(x.type_id.value());
    }
  }

  void operator()(const Symbols& x, Id) {
    for (const auto& [_, symbol] : x.symbols) {
      (*this)(symbol);
    }
  }

  const Graph& graph;
  // ordered map for consistency and sequential processing of related types
  std::map<Type, Info> type_info;
  std::unordered_set<Id> seen;
  std::unordered_set<Id> incomplete;
  Counter nodes;
  Counter types;
  Counter definitions;
  Counter declarations;
};

// Keep track of which type nodes have been unified together, avoiding mapping
// definitions to declarations.
struct UnificationCache {
  UnificationCache(const std::unordered_set<Id>& incomplete, Metrics& metrics)
      : incomplete(incomplete),
        find_query(metrics, "cache.find_query"),
        find_halved(metrics, "cache.find_halved"),
        union_known(metrics, "cache.union_known"),
        union_unknown(metrics, "cache.union_unknown"),
        union_unknown_forced1(metrics, "cache.union_unknown_forced1"),
        union_unknown_forced2(metrics, "cache.union_unknown_forced2") {}

  Id Find(Id id) {
    ++find_query;
    // path halving - tiny performance gain
    while (true) {
      auto it = mapping.find(id);
      if (it == mapping.end()) {
        return id;
      }
      auto& parent = it->second;
      auto parent_it = mapping.find(parent);
      if (parent_it == mapping.end()) {
        return parent;
      }
      auto parent_parent = parent_it->second;
      id = parent = parent_parent;
      ++find_halved;
    }
  }

  void Union(Id id1, Id id2) {
    // no union by rank - overheads result in a performance loss
    const Id fid1 = Find(id1);
    const Id fid2 = Find(id2);
    if (fid1 == fid2) {
      ++union_known;
      return;
    }
    const bool prefer1 = incomplete.find(fid1) == incomplete.end();
    const bool prefer2 = incomplete.find(fid2) == incomplete.end();
    if (prefer1 == prefer2) {
      mapping.insert({fid1, fid2});
      ++union_unknown;
    } else if (prefer1 < prefer2) {
      mapping.insert({fid1, fid2});
      ++union_unknown_forced1;
    } else {
      mapping.insert({fid2, fid1});
      ++union_unknown_forced2;
    }
  }

  const std::unordered_set<Id>& incomplete;
  std::unordered_map<Id, Id> mapping;
  Counter find_query;
  Counter find_halved;
  Counter union_known;
  Counter union_unknown;
  Counter union_unknown_forced1;
  Counter union_unknown_forced2;
};

// Type Unification
//
// This is very similar to Equals. The differences are the recursion control,
// caching and handling of StructUnion and Enum nodes.
//
// During unification, keep track of which pairs of types need to be equal, but
// do not add them to the cache. The caller will do that iff unification
// succeeds.
//
// A declaration and definition of the same named type can be unified. This is
// forward declaration resolution.
struct Unify {
  Unify(const Graph& graph, UnificationCache& cache)
      : graph(graph), cache(cache) {}

  bool operator()(Id id1, Id id2) {
    Id fid1 = Find(id1);
    Id fid2 = Find(id2);
    if (fid1 == fid2) {
      return true;
    }

    // Check if the comparison has an already known result.
    //
    // Opportunistic as seen is unaware of new mappings.
    if (!seen.emplace(fid1, fid2).second) {
      return true;
    }

    if (!graph.Apply2<bool>(*this, fid1, fid2)) {
      return false;
    }

    // These will occasionally get substituted due to a recursive call.
    fid1 = Find(fid1);
    fid2 = Find(fid2);
    if (fid1 == fid2) {
      return true;
    }

    const bool prefer1 = cache.incomplete.find(fid1) == cache.incomplete.end();
    const bool prefer2 = cache.incomplete.find(fid2) == cache.incomplete.end();
    if (prefer1 <= prefer2) {
      mapping.insert({fid1, fid2});
    } else {
      mapping.insert({fid2, fid1});
    }

    return true;
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
                  && x1.name == x2.name;
    // allow mismatches as forward declarations are always unifiable
    if (result && definition1.has_value() && definition2.has_value()) {
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
    bool result = x1.name == x2.name;
    // allow mismatches as forward declarations are always unifiable
    if (result && definition1.has_value() && definition2.has_value()) {
      result = (*this)(definition1->underlying_type_id,
                       definition2->underlying_type_id)
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

  Id Find(Id id) {
    while (true) {
      id = cache.Find(id);
      auto it = mapping.find(id);
      if (it != mapping.end()) {
        id = it->second;
        continue;
      }
      return id;
    }
  }

  const Graph& graph;
  UnificationCache& cache;
  std::unordered_set<Pair> seen;
  std::unordered_map<Id, Id> mapping;
};

}  // namespace

Id ResolveTypes(Graph& graph, Id root, Metrics& metrics) {
  // collect named types
  NamedTypes named_types(graph, metrics);
  {
    const Time time(metrics, "resolve.collection");
    named_types(root);
  }

  UnificationCache cache(named_types.incomplete, metrics);
  {
    const Time time(metrics, "resolve.unification");
    Counter definition_unified(metrics, "resolve.definition.unified");
    Counter definition_not_unified(metrics, "resolve.definition.not_unified");
    Counter declaration_unified(metrics, "resolve.declaration.unified");
    for (auto& [_, info] : named_types.type_info) {
      // try to unify the type definitions
      auto& definitions = info.definitions;
      std::vector<Id> distinct_definitions;
      while (!definitions.empty()) {
        const Id candidate = definitions[0];
        std::vector<Id> todo;
        distinct_definitions.push_back(candidate);
        for (size_t i = 1; i < definitions.size(); ++i) {
          Unify unify(graph, cache);
          if (unify(definitions[i], candidate)) {
            // unification succeeded, commit the mappings
            for (const auto& s : unify.mapping) {
              cache.Union(s.first, s.second);
            }
            ++definition_unified;
          } else {
            // unification failed, conflicting definitions
            todo.push_back(definitions[i]);
            ++definition_not_unified;
          }
        }
        std::swap(todo, definitions);
      }
      // if no conflicts, map all declarations to the definition
      if (distinct_definitions.size() == 1) {
        const Id candidate = distinct_definitions[0];
        for (auto id : info.declarations) {
          cache.Union(id, candidate);
          ++declaration_unified;
        }
      }
    }
  }

  {
    const Time time(metrics, "resolve.rewrite");
    Counter removed(metrics, "resolve.removed");
    Counter retained(metrics, "resolve.retained");
    auto remap = [&cache](Id& id) {
      // update id to representative id, avoiding silent stores
      const Id fid = cache.Find(id);
      if (fid != id) {
        id = fid;
      }
    };
    Substitute<decltype(remap)> substitute(graph, remap);
    for (const auto& id : named_types.seen) {
      const Id fid = cache.Find(id);
      if (fid != id) {
        graph.Remove(id);
        ++removed;
      } else {
        substitute(id);
        ++retained;
      }
    }

    // In case the root node was remapped.
    substitute.Update(root);
  }

  return root;
}

}  // namespace stg
