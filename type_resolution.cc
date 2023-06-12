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

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "error.h"
#include "graph.h"
#include "substitution.h"

namespace stg {

namespace {

// Collect named type definition and declaration nodes.
struct NamedTypes {
  NamedTypes(const Graph& graph, Metrics& metrics)
      : graph(graph),
        seen(graph.MakeDenseIdSet()),
        nodes(metrics, "named_types.nodes"),
        types(metrics, "named_types.types"),
        definitions(metrics, "named_types.definitions"),
        declarations(metrics, "named_types.declarations") {}

  enum class Tag { STRUCT, UNION, ENUM, TYPEDEF };
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

  void operator()(const std::map<std::string, Id>& x) {
    for (const auto& [_, id] : x) {
      (*this)(id);
    }
  }

  // main entry point
  void operator()(Id id) {
    if (seen.Insert(id)) {
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

  void operator()(const PointerToMember& x, Id) {
    (*this)(x.containing_type_id);
    (*this)(x.pointee_type_id);
  }

  void operator()(const Typedef& x, Id id) {
    auto& info = GetInfo(Tag::TYPEDEF, x.name);
    info.definitions.push_back(id);
    ++definitions;
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

  void operator()(const Interface& x, Id) {
    (*this)(x.symbols);
    (*this)(x.types);
  }

  const Graph& graph;
  // ordered map for consistency and sequential processing of related types
  std::map<Type, Info> type_info;
  Graph::DenseIdSet seen;
  Counter nodes;
  Counter types;
  Counter definitions;
  Counter declarations;
};

// Keep track of which type nodes have been unified together, avoiding mapping
// definitions to declarations.
class UnificationCache {
 public:
  UnificationCache(Graph::DenseIdMapping& mapping, Metrics& metrics)
      : mapping_(mapping),
        find_query_(metrics, "cache.find_query"),
        find_halved_(metrics, "cache.find_halved"),
        union_known_(metrics, "cache.union_known"),
        union_unknown_(metrics, "cache.union_unknown") {}

  Id Find(Id id) {
    ++find_query_;
    // path halving - tiny performance gain
    while (true) {
      // note: safe to take references as mapping cannot grow after this
      auto& parent = mapping_[id];
      if (parent == id) {
        return id;
      }
      auto& parent_parent = mapping_[parent];
      if (parent_parent == parent) {
        return parent;
      }
      id = parent = parent_parent;
      ++find_halved_;
    }
  }

  void Union(Id id1, Id id2) {
    // id2 will always be preferred as a parent node; interpreted as a
    // substitution, id1 will be replaced by id2
    const Id fid1 = Find(id1);
    const Id fid2 = Find(id2);
    if (fid1 == fid2) {
      ++union_known_;
      return;
    }
    mapping_[fid1] = fid2;
    ++union_unknown_;
  }

 private:
  Graph::DenseIdMapping& mapping_;
  Counter find_query_;
  Counter find_halved_;
  Counter union_known_;
  Counter union_unknown_;
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
  enum Winner { Neither, Right, Left };  // makes p ? Right : Neither a no-op

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

    const auto winner = graph.Apply2<Winner>(*this, fid1, fid2);
    if (winner == Neither) {
      return false;
    }

    // These will occasionally get substituted due to a recursive call.
    fid1 = Find(fid1);
    fid2 = Find(fid2);
    if (fid1 == fid2) {
      return true;
    }

    if (winner == Left) {
      std::swap(fid1, fid2);
    }
    mapping.insert({fid1, fid2});

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

  Winner operator()(const Void&, const Void&) {
    return Right;
  }

  Winner operator()(const Variadic&, const Variadic&) {
    return Right;
  }

  Winner operator()(const PointerReference& x1,
                    const PointerReference& x2) {
    return x1.kind == x2.kind
        && (*this)(x1.pointee_type_id, x2.pointee_type_id)
        ? Right : Neither;
  }

  Winner operator()(const PointerToMember& x1, const PointerToMember& x2) {
    return (*this)(x1.containing_type_id, x2.containing_type_id)
        && (*this)(x1.pointee_type_id, x2.pointee_type_id)
        ? Right : Neither;
  }

  Winner operator()(const Typedef& x1, const Typedef& x2) {
    return x1.name == x2.name
        && (*this)(x1.referred_type_id, x2.referred_type_id)
        ? Right : Neither;
  }

  Winner operator()(const Qualified& x1, const Qualified& x2) {
    return x1.qualifier == x2.qualifier
        && (*this)(x1.qualified_type_id, x2.qualified_type_id)
        ? Right : Neither;
  }

  Winner operator()(const Primitive& x1, const Primitive& x2) {
    return x1.name == x2.name
        && x1.encoding == x2.encoding
        && x1.bytesize == x2.bytesize
        ? Right : Neither;
  }

  Winner operator()(const Array& x1, const Array& x2) {
    return x1.number_of_elements == x2.number_of_elements
        && (*this)(x1.element_type_id, x2.element_type_id)
        ? Right : Neither;
  }

  Winner operator()(const BaseClass& x1, const BaseClass& x2) {
    return x1.offset == x2.offset
        && x1.inheritance == x2.inheritance
        && (*this)(x1.type_id, x2.type_id)
        ? Right : Neither;
  }

  Winner operator()(const Method& x1, const Method& x2) {
    return x1.mangled_name == x2.mangled_name
        && x1.name == x2.name
        && x1.kind == x2.kind
        && x1.vtable_offset == x2.vtable_offset
        && (*this)(x1.type_id, x2.type_id)
        ? Right : Neither;
  }

  Winner operator()(const Member& x1, const Member& x2) {
    return x1.name == x2.name
        && x1.offset == x2.offset
        && x1.bitsize == x2.bitsize
        && (*this)(x1.type_id, x2.type_id)
        ? Right : Neither;
  }

  Winner operator()(const StructUnion& x1, const StructUnion& x2) {
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
    return result ? definition2.has_value() ? Right : Left : Neither;
  }

  Winner operator()(const Enumeration& x1, const Enumeration& x2) {
    const auto& definition1 = x1.definition;
    const auto& definition2 = x2.definition;
    bool result = x1.name == x2.name;
    // allow mismatches as forward declarations are always unifiable
    if (result && definition1.has_value() && definition2.has_value()) {
      result = (*this)(definition1->underlying_type_id,
                       definition2->underlying_type_id)
               && definition1->enumerators == definition2->enumerators;
    }
    return result ? definition2.has_value() ? Right : Left : Neither;
  }

  Winner operator()(const Function& x1, const Function& x2) {
    return (*this)(x1.parameters, x2.parameters)
        && (*this)(x1.return_type_id, x2.return_type_id)
        ? Right : Neither;
  }

  Winner operator()(const ElfSymbol& x1, const ElfSymbol& x2) {
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
    return result ? Right : Neither;
  }

  Winner operator()(const Interface& x1, const Interface& x2) {
    return (*this)(x1.symbols, x2.symbols)
        && (*this)(x1.types, x2.types)
        ? Right : Neither;
  }

  Winner Mismatch() {
    return Neither;
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

void ResolveTypes(Graph& graph,
                  const std::vector<std::reference_wrapper<Id>>& roots,
                  Metrics& metrics) {
  const Time total(metrics, "resolve.total");

  // collect named types
  NamedTypes named_types(graph, metrics);
  {
    const Time time(metrics, "resolve.collection");
    for (const Id& root : roots) {
      named_types(root);
    }
  }

  Graph::DenseIdMapping mapping = graph.MakeDenseIdMapping();
  UnificationCache cache(mapping, metrics);
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
    named_types.seen.ForEach([&](Id id) {
      const Id fid = cache.Find(id);
      if (fid != id) {
        graph.Remove(id);
        ++removed;
      } else {
        substitute(id);
        ++retained;
      }
    });

    // Update roots
    for (Id& root : roots) {
      substitute.Update(root);
    }
  }
}

}  // namespace stg
