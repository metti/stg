// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2022 Google LLC
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
// Author: Maria Teguiani
// Author: Giuliano Procida
// Author: Ignes Simeonova

#include "stg.h"

#include <algorithm>
#include <array>
#include <ostream>
#include <string_view>
#include <typeinfo>

#include "crc.h"
#include "error.h"
#include "order.h"

namespace stg {

Id ResolveQualifiers(const Graph& graph, Id id, Qualifiers& qualifiers) {
  while (const auto maybe = graph.Get(id).ResolveQualifier(qualifiers))
    id = *maybe;
  return id;
}

Id ResolveTypedefs(
    const Graph& graph, Id id, std::vector<std::string>& typedefs) {
  while (const auto maybe = graph.Get(id).ResolveTypedef(typedefs))
    id = *maybe;
  return id;
}

std::string QualifiersMessage(Qualifier qualifier, const std::string& action) {
  std::ostringstream os;
  os << "qualifier " << qualifier << ' ' << action;
  return os.str();
}

Comparison Removed(State& state, Id node) {
  Comparison comparison{{node}, {}};
  state.outcomes.insert({comparison, {}});
  return comparison;
}

Comparison Added(State& state, Id node) {
  Comparison comparison{{}, {node}};
  state.outcomes.insert({comparison, {}});
  return comparison;
}

/*
 * We compute a diff for every visited node.
 *
 * Each node has one of:
 * 1. equals = true; perhaps only tentative edge differences
 * 2. equals = false; at least one definitive node or edge difference
 *
 * On the first visit to a node we can put a placeholder in, the equals value is
 * irrelevant, the diff may contain local and edge differences. If an SCC
 * contains only internal edge differences (and equivalently equals is true)
 * then the differences can all (eventually) be discarded.
 *
 * On exit from the first visit to a node, equals reflects the tree of
 * comparisons below that node in the DFS and similarly, the diff graph starting
 * from the node contains a subtree of this tree plus potentially edges to
 * existing nodes to the side or below (already visited SCCs, sharing), or above
 * (back links forming cycles).
 *
 * When an SCC is closed, all equals implies deleting all diffs, any false
 * implies updating all to false.
 *
 * On subsequent visits to a node, there are 2 cases. The node is still open:
 * return true and an edge diff. The node is closed, return the stored value and
 * an edge diff.
 */
std::pair<bool, std::optional<Comparison>> Compare(
    State& state, Id node1, Id node2) {
  const Comparison comparison{{node1}, {node2}};

  // 1. Check if the comparison has an already known result.
  auto already_known = state.known.find(comparison);
  if (already_known != state.known.end()) {
    // Already visited and closed.
    if (already_known->second)
      return {true, {}};
    else
      return {false, {comparison}};
  }
  // Either open or not visited at all

  // 2. Record node with Strongly-Connected Component finder.
  auto handle = state.scc.Open(comparison);
  if (!handle) {
    // Already open.
    //
    // Return a dummy true outcome and some tentative diffs. The diffs may end
    // up not being used and, while it would be nice to be lazier, they encode
    // all the cycling-breaking edges needed to recreate a full diff structure.
    return {true, {comparison}};
  }
  // Comparison opened, need to close it before returning.

  const Graph& graph = state.graph;
  Result result;

  Qualifiers qualifiers1;
  Qualifiers qualifiers2;
  const Id unqualified1 = ResolveQualifiers(graph, node1, qualifiers1);
  const Id unqualified2 = ResolveQualifiers(graph, node2, qualifiers2);
  if (!qualifiers1.empty() || !qualifiers2.empty()) {
    // 3.1 Qualified type difference.
    auto it1 = qualifiers1.begin();
    auto it2 = qualifiers2.begin();
    const auto end1 = qualifiers1.end();
    const auto end2 = qualifiers2.end();
    while (it1 != end1 || it2 != end2) {
      if (it2 == end2 || (it1 != end1 && *it1 < *it2)) {
        result.AddNodeDiff(QualifiersMessage(*it1, "removed"));
        ++it1;
      } else if (it1 == end1 || (it2 != end2 && *it1 > *it2)) {
        result.AddNodeDiff(QualifiersMessage(*it2, "added"));
        ++it2;
      } else {
        ++it1;
        ++it2;
      }
    }
    const auto type_diff = Compare(state, unqualified1, unqualified2);
    result.MaybeAddEdgeDiff("underlying", type_diff);
  } else {
    std::vector<std::string> typedefs1;
    std::vector<std::string> typedefs2;
    const Id resolved1 = ResolveTypedefs(graph, unqualified1, typedefs1);
    const Id resolved2 = ResolveTypedefs(graph, unqualified2, typedefs2);
    if (unqualified1 != resolved1 || unqualified2 != resolved2) {
      // 3.2 Typedef difference.
      result.diff_.holds_changes = !typedefs1.empty() && !typedefs2.empty()
                                   && typedefs1[0] == typedefs2[0];
      result.MaybeAddEdgeDiff("resolved", Compare(state, resolved1, resolved2));
    } else {
      const auto& type1 = graph.Get(unqualified1);
      const auto& type2 = graph.Get(unqualified2);
      if (typeid(type1) != typeid(type2)) {
        // 4. Incomparable.
        result.MarkIncomparable();
      } else {
        // 5. Actually compare with dynamic type dispatch.
        result = type1.Equals(state, type2);
      }
    }
  }

  // 6. Update result and check for a complete Strongly-Connected Component.
  state.provisional.insert({comparison, result.diff_});
  auto comparisons = state.scc.Close(*handle);
  if (!comparisons.empty()) {
    // Closed SCC.
    //
    // Note that result now incorporates every inequality and difference in the
    // SCC via the DFS spanning tree.
    for (auto& c : comparisons) {
      // Record equality / inequality.
      state.known.insert({c, result.equals_});
      const auto it = state.provisional.find(c);
      Check(it != state.provisional.end())
          << "internal error: missing provisional diffs";
      if (!result.equals_)
        // Record differences.
        state.outcomes.insert(*it);
      state.provisional.erase(it);
    }
    if (result.equals_)
      return {true, {}};
    else
      return {false, {comparison}};
  }

  // Note that both equals and diff are tentative as comparison is still open.
  return {result.equals_, {comparison}};
}

Result Void::Equals(State&, const Node&) const { return {}; }

Result Variadic::Equals(State&, const Node&) const { return {}; }

Result PointerReference::Equals(State& state, const Node& other) const {
  const auto& o = other.as<PointerReference>();

  Result result;
  if (kind != o.kind)
    return result.MarkIncomparable();
  const auto type_diff = Compare(state, pointee_type_id, o.pointee_type_id);
  const auto text =
      kind == PointerReference::Kind::POINTER ? "pointed-to" : "referred-to";
  result.MaybeAddEdgeDiff(text, type_diff);
  return result;
}

Result Typedef::Equals(State&, const Node&) const {
  // Compare will never attempt to directly compare Typedefs.
  Die() << "internal error: Typedef::Equals";
}

Result Qualified::Equals(State&, const Node&) const {
  // Compare will never attempt to directly compare Qualifiers.
  Die() << "internal error: Qualified::Equals";
}

Result Primitive::Equals(State&, const Node& other) const {
  const auto& o = other.as<Primitive>();

  Result result;
  if (name != o.name) {
    return result.MarkIncomparable();
  }
  result.diff_.holds_changes = !name.empty();
  result.MaybeAddNodeDiff("encoding", encoding, o.encoding);
  result.MaybeAddNodeDiff("bit size", bitsize, o.bitsize);
  if (bitsize != bytesize * 8 && o.bitsize != o.bytesize * 8)
    result.MaybeAddNodeDiff("byte size", bytesize, o.bytesize);
  return result;
}

Result Array::Equals(State& state, const Node& other) const {
  const auto& o = other.as<Array>();

  Result result;
  result.MaybeAddNodeDiff("number of elements",
                          number_of_elements, o.number_of_elements);
  const auto type_diff = Compare(state, element_type_id, o.element_type_id);
  result.MaybeAddEdgeDiff("element", type_diff);
  return result;
}

static bool CompareDefined(bool defined1, bool defined2, Result& result,
                           bool ignore_diff) {
  if (defined1 && defined2)
    return true;
  if (!ignore_diff && defined1 != defined2) {
    std::ostringstream os;
    os << "was " << (defined1 ? "fully defined" : "only declared")
       << ", is now " << (defined2 ? "fully defined" : "only declared");
    result.AddNodeDiff(os.str());
  }
  return false;
}

using KeyIndexPairs = std::vector<std::pair<std::string, size_t>>;
static KeyIndexPairs MatchingKeys(const Graph& graph,
                                  const std::vector<Id>& nodes) {
  KeyIndexPairs keys;
  const auto size = nodes.size();
  keys.reserve(size);
  size_t anonymous_ix = 0;
  for (size_t ix = 0; ix < size; ++ix) {
    auto key = graph.Get(nodes[ix]).MatchingKey(graph);
    if (key.empty())
      key = "#anon#" + std::to_string(anonymous_ix++);
    keys.push_back({key, ix});
  }
  std::stable_sort(keys.begin(), keys.end());
  return keys;
}

using MatchedPairs =
    std::vector<std::pair<std::optional<size_t>, std::optional<size_t>>>;
static MatchedPairs PairUp(const KeyIndexPairs& keys1,
                           const KeyIndexPairs& keys2) {
  MatchedPairs pairs;
  pairs.reserve(std::max(keys1.size(), keys2.size()));
  auto it1 = keys1.begin();
  auto it2 = keys2.begin();
  const auto end1 = keys1.end();
  const auto end2 = keys2.end();
  while (it1 != end1 || it2 != end2) {
    if (it2 == end2 || (it1 != end1 && it1->first < it2->first)) {
      // removed
      pairs.push_back({{it1->second}, {}});
      ++it1;
    } else if (it1 == end1 || (it2 != end2 && it1->first > it2->first)) {
      // added
      pairs.push_back({{}, {it2->second}});
      ++it2;
    } else {
      // in both
      pairs.push_back({{it1->second}, {it2->second}});
      ++it1;
      ++it2;
    }
  }
  return pairs;
}

static void CompareNodes(Result& result, State& state,
                         const std::vector<Id>& nodes1,
                         const std::vector<Id>& nodes2,
                         const bool reorder) {
  const auto keys1 = MatchingKeys(state.graph, nodes1);
  const auto keys2 = MatchingKeys(state.graph, nodes2);
  auto pairs = PairUp(keys1, keys2);
  if (reorder)
    Reorder(pairs);
  for (const auto& [index1, index2] : pairs) {
    if (index1 && !index2) {
      // removed
      const auto& node1 = nodes1[*index1];
      result.AddEdgeDiff("", Removed(state, node1));
    } else if (!index1 && index2) {
      // added
      const auto& node2 = nodes2[*index2];
      result.AddEdgeDiff("", Added(state, node2));
    } else {
      // in both
      const auto& node1 = nodes1[*index1];
      const auto& node2 = nodes2[*index2];
      result.MaybeAddEdgeDiff("", Compare(state, node1, node2));
    }
  }
}

Result BaseClass::Equals(State& state, const Node& other) const {
  const auto& o = other.as<BaseClass>();

  Result result;
  result.MaybeAddNodeDiff("inheritance", inheritance, o.inheritance);
  result.MaybeAddNodeDiff("offset", offset, o.offset);
  result.MaybeAddEdgeDiff("", Compare(state, type_id, o.type_id));
  return result;
}

Result Member::Equals(State& state, const Node& other) const {
  const auto& o = other.as<Member>();

  Result result;
  result.MaybeAddNodeDiff("offset", offset, o.offset);
  result.MaybeAddNodeDiff("size", bitsize, o.bitsize);
  result.MaybeAddEdgeDiff("", Compare(state, type_id, o.type_id));
  return result;
}

Result Method::Equals(State& state, const Node& other) const {
  const auto& o = other.as<Method>();

  Result result;
  result.MaybeAddNodeDiff("kind", kind, o.kind);
  result.MaybeAddNodeDiff("vtable offset", vtable_offset, o.vtable_offset);
  result.MaybeAddEdgeDiff("", Compare(state, type_id, o.type_id));
  return result;
}

Result StructUnion::Equals(State& state, const Node& other) const {
  const auto& o = other.as<StructUnion>();

  Result result;
  // Compare two anonymous types recursively, not holding diffs.
  // Compare two identically named types recursively, holding diffs.
  // Everything else treated as distinct. No recursion.
  if (kind != o.kind || name != o.name)
    return result.MarkIncomparable();
  result.diff_.holds_changes = !name.empty();

  const auto& definition1 = definition;
  const auto& definition2 = o.definition;
  if (!CompareDefined(definition1.has_value(), definition2.has_value(), result,
                      state.options.ignore_type_declaration_status_changes))
    return result;

  result.MaybeAddNodeDiff(
      "byte size", definition1->bytesize, definition2->bytesize);
  CompareNodes(
     result, state, definition1->base_classes, definition2->base_classes, true);
  CompareNodes(
     result, state, definition1->methods, definition2->methods, false);
  CompareNodes(result, state, definition1->members, definition2->members, true);

  return result;
}

Result Enumeration::Equals(State& state, const Node& other) const {
  const auto& o = other.as<Enumeration>();

  Result result;
  // Compare two anonymous types recursively, not holding diffs.
  // Compare two identically named types recursively, holding diffs.
  // Everything else treated as distinct. No recursion.
  if (name != o.name)
    return result.MarkIncomparable();
  result.diff_.holds_changes = !name.empty();

  const auto& definition1 = definition;
  const auto& definition2 = o.definition;
  if (!CompareDefined(definition1.has_value(), definition2.has_value(), result,
                      state.options.ignore_type_declaration_status_changes))
    return result;
  result.MaybeAddNodeDiff(
      "byte size", definition1->bytesize, definition2->bytesize);

  const auto enums1 = definition1->enumerators;
  const auto enums2 = definition2->enumerators;
  const auto names1 = GetEnumNames();
  const auto names2 = o.GetEnumNames();
  auto pairs = PairUp(names1, names2);
  Reorder(pairs);
  for (const auto& [index1, index2] : pairs) {
    if (index1 && !index2) {
      // removed
      const auto& enum1 = enums1[*index1];
      std::ostringstream os;
      os << "enumerator '" << enum1.first
         << "' (" << enum1.second << ") was removed";
      result.AddNodeDiff(os.str());
    } else if (!index1 && index2) {
      // added
      const auto& enum2 = enums2[*index2];
      std::ostringstream os;
      os << "enumerator '" << enum2.first
         << "' (" << enum2.second << ") was added";
      result.AddNodeDiff(os.str());
    } else {
      // in both
      const auto& enum1 = enums1[*index1];
      const auto& enum2 = enums2[*index2];
      result.MaybeAddNodeDiff(
          [&](std::ostream& os) {
            os << "enumerator '" << enum1.first << "' value";
          },
          enum1.second, enum2.second);
    }
  }

  return result;
}

Result Function::Equals(State& state, const Node& other) const {
  const auto& o = other.as<Function>();

  Result result;
  const auto type_diff = Compare(state, return_type_id, o.return_type_id);
  result.MaybeAddEdgeDiff("return", type_diff);

  const auto& parameters1 = parameters;
  const auto& parameters2 = o.parameters;
  size_t min = std::min(parameters1.size(), parameters2.size());
  for (size_t i = 0; i < min; ++i) {
    const Id p1 = parameters1.at(i);
    const Id p2 = parameters2.at(i);
    result.MaybeAddEdgeDiff(
        [&](std::ostream& os) {
          os << "parameter " << i + 1;
        },
        Compare(state, p1, p2));
  }

  bool added = parameters1.size() < parameters2.size();
  const auto& which = added ? o : *this;
  const auto& parameters = which.parameters;
  for (size_t i = min; i < parameters.size(); ++i) {
    const Id parameter = parameters.at(i);
    std::ostringstream os;
    os << "parameter " << i + 1 << " of";
    auto diff = added ? Added(state, parameter) : Removed(state, parameter);
    result.AddEdgeDiff(os.str(), diff);
  }

  return result;
}

Result ElfSymbol::Equals(State& state, const Node& other) const {
  const auto& o = other.as<ElfSymbol>();

  // ELF symbols have a lot of different attributes that can impact ABI
  // compatibility and others that either cannot or are subsumed by information
  // elsewhere.
  //
  // Not all attributes are exposed by elf_symbol and fewer still in ABI XML.
  //
  // name - definitely part of the key
  //
  // type - (ELF symbol type, not C type) one important distinction here would
  // be global vs thread-local variables
  //
  // section - not exposed (modulo aliasing information) and don't care
  //
  // value (address, usually) - not exposed (modulo aliasing information) and
  // don't care
  //
  // size - don't care (for variables, subsumed by type information)
  //
  // binding - global vs weak vs unique vs local
  //
  // visibility - default > protected > hidden > internal
  //
  // version / is-default-version - in theory the "hidden" bit (separate from
  // hidden and local above) can be set independently of the version, but in
  // practice at most one version of given name is non-hidden; version
  // (including its presence or absence) is definitely part of the key; we
  // should probably treat is-default-version as a non-key attribute
  //
  // defined - rather fundamental; libabigail currently doesn't track undefined
  // symbols but we should obviously be prepared in case it does

  // There are also some externalities which libabigail cares about, which may
  // or may not be exposed in the XML
  //
  // index - don't care
  //
  // is-common and friends - don't care
  //
  // aliases - exposed, but we don't really care; however we should see what
  // compilers do, if anything, in terms of propagating type information to
  // aliases

  // Linux kernel things.
  //
  // MODVERSIONS CRC - fundamental to ABI compatibility, if present
  //
  // Symbol namespace - fundamental to ABI compatibility, if present

  Result result;
  result.MaybeAddNodeDiff("name", symbol_name, o.symbol_name);

  if (version_info && o.version_info) {
    result.MaybeAddNodeDiff("version", version_info->name,
                            o.version_info->name);
    result.MaybeAddNodeDiff("default version", version_info->is_default,
                            o.version_info->is_default);
  } else {
    result.MaybeAddNodeDiff("has version", version_info.has_value(),
                            o.version_info.has_value());
  }

  result.MaybeAddNodeDiff("defined", is_defined, o.is_defined);
  result.MaybeAddNodeDiff("symbol type", symbol_type, o.symbol_type);
  result.MaybeAddNodeDiff("binding", binding, o.binding);
  result.MaybeAddNodeDiff("visibility", visibility, o.visibility);
  result.MaybeAddNodeDiff("CRC", crc, o.crc);
  result.MaybeAddNodeDiff("namespace", ns, o.ns);

  if (type_id && o.type_id) {
    result.MaybeAddEdgeDiff("", Compare(state, *type_id, *o.type_id));
  } else if (type_id) {
    if (!state.options.ignore_symbol_type_presence_changes)
      result.AddEdgeDiff("", Removed(state, *type_id));
  } else if (o.type_id) {
    if (!state.options.ignore_symbol_type_presence_changes)
      result.AddEdgeDiff("", Added(state, *o.type_id));
  } else {
    // both types missing, we have nothing to say
  }

  return result;
}

Result Symbols::Equals(State& state, const Node& other) const {
  const auto& o = other.as<Symbols>();

  Result result;
  result.diff_.holds_changes = true;

  // Group diffs into removed, added and changed symbols for readability.
  std::vector<Id> removed;
  std::vector<Id> added;
  std::vector<std::pair<Id, Id>> in_both;

  const auto& symbols1 = symbols;
  const auto& symbols2 = o.symbols;
  auto it1 = symbols1.begin();
  auto it2 = symbols2.begin();
  const auto end1 = symbols1.end();
  const auto end2 = symbols2.end();
  while (it1 != end1 || it2 != end2) {
    if (it2 == end2 || (it1 != end1 && it1->first < it2->first)) {
      // removed
      removed.push_back(it1->second);
      ++it1;
    } else if (it1 == end1 || (it2 != end2 && it1->first > it2->first)) {
      // added
      added.push_back(it2->second);
      ++it2;
    } else {
      // in both
      in_both.push_back({it1->second, it2->second});
      ++it1;
      ++it2;
    }
  }

  for (const auto symbol1 : removed)
    result.AddEdgeDiff("", Removed(state, symbol1));
  for (const auto symbol2 : added)
    result.AddEdgeDiff("", Added(state, symbol2));
  for (const auto& [symbol1, symbol2] : in_both)
    result.MaybeAddEdgeDiff("", Compare(state, symbol1, symbol2));

  return result;
}

std::optional<Id> Node::ResolveQualifier(Qualifiers&) const {
  return {};
}

std::optional<Id> Array::ResolveQualifier(Qualifiers& qualifiers) const {
  // There should be no qualifiers here.
  qualifiers.clear();
  return {};
}

std::optional<Id> Function::ResolveQualifier(Qualifiers& qualifiers) const {
  // There should be no qualifiers here.
  qualifiers.clear();
  return {};
}

std::optional<Id> Qualified::ResolveQualifier(Qualifiers& qualifiers) const {
  qualifiers.insert(qualifier);
  return {qualified_type_id};
}

std::optional<Id> Node::ResolveTypedef(std::vector<std::string>&) const {
  return {};
}

std::optional<Id> Typedef::ResolveTypedef(
    std::vector<std::string>& typedefs) const {
  typedefs.push_back(name);
  return {referred_type_id};
}

std::string Node::MatchingKey(const Graph&) const { return {}; }

std::string BaseClass::MatchingKey(const Graph& graph) const {
  return graph.Get(type_id).MatchingKey(graph);
}

std::string Member::MatchingKey(const Graph& graph) const {
  if (!name.empty())
    return name;
  return graph.Get(type_id).MatchingKey(graph);
}

std::string Method::MatchingKey(const Graph&) const {
  return name + ',' + mangled_name;
}

std::string StructUnion::MatchingKey(const Graph& graph) const {
  if (!name.empty())
    return name;
  if (definition) {
    const auto& members = definition->members;
    for (const auto& member : members) {
      const auto recursive = graph.Get(member).MatchingKey(graph);
      if (!recursive.empty())
        return recursive;
    }
  }
  return {};
}

std::vector<std::pair<std::string, size_t>> Enumeration::GetEnumNames() const {
  std::vector<std::pair<std::string, size_t>> names;
  if (definition) {
    const auto& enums = definition->enumerators;
    const auto size = enums.size();
    names.reserve(size);
    for (size_t ix = 0; ix < size; ++ix) {
      const auto& name = enums[ix].first;
      names.push_back({name, ix});
    }
    std::stable_sort(names.begin(), names.end());
  }
  return names;
}

std::ostream& operator<<(std::ostream& os, BaseClass::Inheritance inheritance) {
  switch (inheritance) {
    case BaseClass::Inheritance::NON_VIRTUAL:
      return os << "non-virtual";
    case BaseClass::Inheritance::VIRTUAL:
      return os << "virtual";
  }
}

std::ostream& operator<<(std::ostream& os, Method::Kind kind) {
  switch (kind) {
    case Method::Kind::NON_VIRTUAL:
      return os << "non-virtual";
    case Method::Kind::STATIC:
      return os << "static";
    case Method::Kind::VIRTUAL:
      return os << "virtual";
  }
}

std::ostream& operator<<(std::ostream& os, StructUnion::Kind kind) {
  switch (kind) {
    case StructUnion::Kind::CLASS:
      return os << "class";
    case StructUnion::Kind::STRUCT:
      return os << "struct";
    case StructUnion::Kind::UNION:
      return os << "union";
  }
}

std::ostream& operator<<(std::ostream& os, Qualifier qualifier) {
  switch (qualifier) {
    case Qualifier::CONST:
      return os << "const";
    case Qualifier::VOLATILE:
      return os << "volatile";
    case Qualifier::RESTRICT:
      return os << "restrict";
  }
}

std::ostream& operator<<(std::ostream& os, ElfSymbol::SymbolType type) {
  switch (type) {
    case ElfSymbol::SymbolType::OBJECT:
      return os << "variable";
    case ElfSymbol::SymbolType::FUNCTION:
      return os << "function";
    case ElfSymbol::SymbolType::COMMON:
      return os << "common";
    case ElfSymbol::SymbolType::TLS:
      return os << "TLS";
  }
}

std::ostream& operator<<(std::ostream& os, ElfSymbol::Binding binding) {
  switch (binding) {
    case ElfSymbol::Binding::GLOBAL:
      return os << "global";
    case ElfSymbol::Binding::LOCAL:
      return os << "local";
    case ElfSymbol::Binding::WEAK:
      return os << "weak";
    case ElfSymbol::Binding::GNU_UNIQUE:
      return os << "GNU unique";
  }
}

std::ostream& operator<<(std::ostream& os, ElfSymbol::Visibility visibility) {
  switch (visibility) {
    case ElfSymbol::Visibility::DEFAULT:
      return os << "default";
    case ElfSymbol::Visibility::PROTECTED:
      return os << "protected";
    case ElfSymbol::Visibility::HIDDEN:
      return os << "hidden";
    case ElfSymbol::Visibility::INTERNAL:
      return os << "internal";
  }
}

std::string VersionInfoToString(const ElfSymbol::VersionInfo& version_info) {
  return '@' + std::string(version_info.is_default ? "@" : "") +
         version_info.name;
}

std::ostream& operator<<(std::ostream& os, Primitive::Encoding encoding) {
  switch (encoding) {
    case Primitive::Encoding::BOOLEAN:
      return os << "boolean";
    case Primitive::Encoding::SIGNED_INTEGER:
      return os << "signed integer";
    case Primitive::Encoding::UNSIGNED_INTEGER:
      return os << "unsigned integer";
    case Primitive::Encoding::SIGNED_CHARACTER:
      return os << "signed character";
    case Primitive::Encoding::UNSIGNED_CHARACTER:
      return os << "unsigned character";
    case Primitive::Encoding::REAL_NUMBER:
      return os << "real number";
    case Primitive::Encoding::COMPLEX_NUMBER:
      return os << "complex number";
    case Primitive::Encoding::UTF:
      return os << "UTF";
  }
}

std::ostream& operator<<(std::ostream& os, PointerReference::Kind kind) {
  switch (kind) {
    case PointerReference::Kind::POINTER:
      return os << "pointer";
    case PointerReference::Kind::LVALUE_REFERENCE:
      return os << "lvalue reference";
    case PointerReference::Kind::RVALUE_REFERENCE:
      return os << "rvalue reference";
  }
}

}  // namespace stg
