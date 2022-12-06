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

#include "comparison.h"

#include <algorithm>
#include <ostream>

#include "error.h"
#include "order.h"

namespace stg {

std::string QualifiersMessage(Qualifier qualifier, const std::string& action) {
  std::ostringstream os;
  os << "qualifier " << qualifier << ' ' << action;
  return os.str();
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
std::pair<bool, std::optional<Comparison>> Compare::operator()(Id id1, Id id2) {
  const Comparison comparison{{id1}, {id2}};

  // 1. Check if the comparison has an already known result.
  auto already_known = known.find(comparison);
  if (already_known != known.end()) {
    // Already visited and closed.
    if (already_known->second)
      return {true, {}};
    else
      return {false, {comparison}};
  }
  // Either open or not visited at all

  // 2. Record node with Strongly-Connected Component finder.
  auto handle = scc.Open(comparison);
  if (!handle) {
    // Already open.
    //
    // Return a dummy true outcome and some tentative diffs. The diffs may end
    // up not being used and, while it would be nice to be lazier, they encode
    // all the cycling-breaking edges needed to recreate a full diff structure.
    return {true, {comparison}};
  }
  // Comparison opened, need to close it before returning.

  Result result;

  const auto [unqualified1, qualifiers1] = ResolveQualifiers(graph, id1);
  const auto [unqualified2, qualifiers2] = ResolveQualifiers(graph, id2);
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
    const auto type_diff = (*this)(unqualified1, unqualified2);
    result.MaybeAddEdgeDiff("underlying", type_diff);
  } else {
    const auto [resolved1, typedefs1] = ResolveTypedefs(graph, unqualified1);
    const auto [resolved2, typedefs2] = ResolveTypedefs(graph, unqualified2);
    if (unqualified1 != resolved1 || unqualified2 != resolved2) {
      // 3.2 Typedef difference.
      result.diff_.holds_changes = !typedefs1.empty() && !typedefs2.empty()
                                   && typedefs1[0] == typedefs2[0];
      result.MaybeAddEdgeDiff("resolved", (*this)(resolved1, resolved2));
    } else {
      // 4. Compare nodes, if possible.
      result = graph.Apply2<Result>(*this, unqualified1, unqualified2);
    }
  }

  // 5. Update result and check for a complete Strongly-Connected Component.
  provisional.insert({comparison, result.diff_});
  auto comparisons = scc.Close(*handle);
  if (!comparisons.empty()) {
    // Closed SCC.
    //
    // Note that result now incorporates every inequality and difference in the
    // SCC via the DFS spanning tree.
    for (auto& c : comparisons) {
      // Record equality / inequality.
      known.insert({c, result.equals_});
      const auto it = provisional.find(c);
      Check(it != provisional.end())
          << "internal error: missing provisional diffs";
      if (!result.equals_)
        // Record differences.
        outcomes.insert(*it);
      provisional.erase(it);
    }
    if (result.equals_)
      return {true, {}};
    else
      return {false, {comparison}};
  }

  // Note that both equals and diff are tentative as comparison is still open.
  return {result.equals_, {comparison}};
}

Comparison Compare::Removed(Id id) {
  Comparison comparison{{id}, {}};
  outcomes.insert({comparison, {}});
  return comparison;
}

Comparison Compare::Added(Id id) {
  Comparison comparison{{}, {id}};
  outcomes.insert({comparison, {}});
  return comparison;
}

Result Compare::Mismatch() {
  return Result().MarkIncomparable();
}

Result Compare::operator()(const Void&, const Void&) {
  return {};
}

Result Compare::operator()(const Variadic&, const Variadic&) {
  return {};
}

Result Compare::operator()(const PointerReference& x1,
                           const PointerReference& x2) {
  Result result;
  if (x1.kind != x2.kind)
    return result.MarkIncomparable();
  const auto type_diff = (*this)(x1.pointee_type_id, x2.pointee_type_id);
  const auto text =
      x1.kind == PointerReference::Kind::POINTER ? "pointed-to" : "referred-to";
  result.MaybeAddEdgeDiff(text, type_diff);
  return result;
}

Result Compare::operator()(const Typedef&, const Typedef&) {
  // Compare will never attempt to directly compare Typedefs.
  Die() << "internal error: Compare(Typedef)";
}

Result Compare::operator()(const Qualified&, const Qualified&) {
  // Compare will never attempt to directly compare Qualifiers.
  Die() << "internal error: Compare(Qualified)";
}

Result Compare::operator()(const Primitive& x1, const Primitive& x2) {
  Result result;
  if (x1.name != x2.name) {
    return result.MarkIncomparable();
  }
  result.diff_.holds_changes = !x1.name.empty();
  result.MaybeAddNodeDiff("encoding", x1.encoding, x2.encoding);
  result.MaybeAddNodeDiff("bit size", x1.bitsize, x2.bitsize);
  if (x1.bitsize != x1.bytesize * 8 && x2.bitsize != x2.bytesize * 8)
    result.MaybeAddNodeDiff("byte size", x1.bytesize, x2.bytesize);
  return result;
}

Result Compare::operator()(const Array& x1, const Array& x2) {
  Result result;
  result.MaybeAddNodeDiff("number of elements",
                          x1.number_of_elements, x2.number_of_elements);
  const auto type_diff = (*this)(x1.element_type_id, x2.element_type_id);
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
                                  const std::vector<Id>& ids) {
  KeyIndexPairs keys;
  const auto size = ids.size();
  keys.reserve(size);
  size_t anonymous_ix = 0;
  for (size_t ix = 0; ix < size; ++ix) {
    auto key = MatchingKey(graph)(ids[ix]);
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

static void CompareNodes(Result& result, Compare& compare,
                         const std::vector<Id>& ids1,
                         const std::vector<Id>& ids2,
                         const bool reorder) {
  const auto keys1 = MatchingKeys(compare.graph, ids1);
  const auto keys2 = MatchingKeys(compare.graph, ids2);
  auto pairs = PairUp(keys1, keys2);
  if (reorder)
    Reorder(pairs);
  for (const auto& [index1, index2] : pairs) {
    if (index1 && !index2) {
      // removed
      const auto& x1 = ids1[*index1];
      result.AddEdgeDiff("", compare.Removed(x1));
    } else if (!index1 && index2) {
      // added
      const auto& x2 = ids2[*index2];
      result.AddEdgeDiff("", compare.Added(x2));
    } else {
      // in both
      const auto& x1 = ids1[*index1];
      const auto& x2 = ids2[*index2];
      result.MaybeAddEdgeDiff("", compare(x1, x2));
    }
  }
}

Result Compare::operator()(const BaseClass& x1, const BaseClass& x2) {
  Result result;
  result.MaybeAddNodeDiff("inheritance", x1.inheritance, x2.inheritance);
  result.MaybeAddNodeDiff("offset", x1.offset, x2.offset);
  result.MaybeAddEdgeDiff("", (*this)(x1.type_id, x2.type_id));
  return result;
}

Result Compare::operator()(const Member& x1, const Member& x2) {
  Result result;
  result.MaybeAddNodeDiff("offset", x1.offset, x2.offset);
  result.MaybeAddNodeDiff("size", x1.bitsize, x2.bitsize);
  result.MaybeAddEdgeDiff("", (*this)(x1.type_id, x2.type_id));
  return result;
}

Result Compare::operator()(const Method& x1, const Method& x2) {
  Result result;
  result.MaybeAddNodeDiff("kind", x1.kind, x2.kind);
  result.MaybeAddNodeDiff("vtable offset", x1.vtable_offset, x2.vtable_offset);
  result.MaybeAddEdgeDiff("", (*this)(x1.type_id, x2.type_id));
  return result;
}

Result Compare::operator()(const StructUnion& x1, const StructUnion& x2) {
  Result result;
  // Compare two anonymous types recursively, not holding diffs.
  // Compare two identically named types recursively, holding diffs.
  // Everything else treated as distinct. No recursion.
  if (x1.kind != x2.kind || x1.name != x2.name)
    return result.MarkIncomparable();
  result.diff_.holds_changes = !x1.name.empty();

  const auto& definition1 = x1.definition;
  const auto& definition2 = x2.definition;
  if (!CompareDefined(definition1.has_value(), definition2.has_value(), result,
                      options.ignore_type_declaration_status_changes))
    return result;

  result.MaybeAddNodeDiff(
      "byte size", definition1->bytesize, definition2->bytesize);
  CompareNodes(
     result, *this, definition1->base_classes, definition2->base_classes, true);
  CompareNodes(
     result, *this, definition1->methods, definition2->methods, false);
  CompareNodes(result, *this, definition1->members, definition2->members, true);

  return result;
}

static KeyIndexPairs MatchingKeys(const Enumeration::Enumerators& enums) {
  KeyIndexPairs names;
  const auto size = enums.size();
  names.reserve(size);
  for (size_t ix = 0; ix < size; ++ix) {
    const auto& name = enums[ix].first;
    names.push_back({name, ix});
  }
  std::stable_sort(names.begin(), names.end());
  return names;
}

Result Compare::operator()(const Enumeration& x1, const Enumeration& x2) {
  Result result;
  // Compare two anonymous types recursively, not holding diffs.
  // Compare two identically named types recursively, holding diffs.
  // Everything else treated as distinct. No recursion.
  if (x1.name != x2.name)
    return result.MarkIncomparable();
  result.diff_.holds_changes = !x1.name.empty();

  const auto& definition1 = x1.definition;
  const auto& definition2 = x2.definition;
  if (!CompareDefined(definition1.has_value(), definition2.has_value(), result,
                      options.ignore_type_declaration_status_changes))
    return result;
  result.MaybeAddNodeDiff(
      "byte size", definition1->bytesize, definition2->bytesize);

  const auto enums1 = definition1->enumerators;
  const auto enums2 = definition2->enumerators;
  const auto keys1 = MatchingKeys(enums1);
  const auto keys2 = MatchingKeys(enums2);
  auto pairs = PairUp(keys1, keys2);
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

Result Compare::operator()(const Function& x1, const Function& x2) {
  Result result;
  const auto type_diff = (*this)(x1.return_type_id, x2.return_type_id);
  result.MaybeAddEdgeDiff("return", type_diff);

  const auto& parameters1 = x1.parameters;
  const auto& parameters2 = x2.parameters;
  size_t min = std::min(parameters1.size(), parameters2.size());
  for (size_t i = 0; i < min; ++i) {
    const Id p1 = parameters1.at(i);
    const Id p2 = parameters2.at(i);
    result.MaybeAddEdgeDiff(
        [&](std::ostream& os) {
          os << "parameter " << i + 1;
        },
        (*this)(p1, p2));
  }

  bool added = parameters1.size() < parameters2.size();
  const auto& which = added ? x2 : x1;
  const auto& parameters = which.parameters;
  for (size_t i = min; i < parameters.size(); ++i) {
    const Id parameter = parameters.at(i);
    std::ostringstream os;
    os << "parameter " << i + 1 << " of";
    auto diff = added ? Added(parameter) : Removed(parameter);
    result.AddEdgeDiff(os.str(), diff);
  }

  return result;
}

Result Compare::operator()(const ElfSymbol& x1, const ElfSymbol& x2) {
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
  result.MaybeAddNodeDiff("name", x1.symbol_name, x2.symbol_name);

  if (x1.version_info && x2.version_info) {
    result.MaybeAddNodeDiff("version", x1.version_info->name,
                            x2.version_info->name);
    result.MaybeAddNodeDiff("default version", x1.version_info->is_default,
                            x2.version_info->is_default);
  } else {
    result.MaybeAddNodeDiff("has version", x1.version_info.has_value(),
                            x2.version_info.has_value());
  }

  result.MaybeAddNodeDiff("defined", x1.is_defined, x2.is_defined);
  result.MaybeAddNodeDiff("symbol type", x1.symbol_type, x2.symbol_type);
  result.MaybeAddNodeDiff("binding", x1.binding, x2.binding);
  result.MaybeAddNodeDiff("visibility", x1.visibility, x2.visibility);
  result.MaybeAddNodeDiff("CRC", x1.crc, x2.crc);
  result.MaybeAddNodeDiff("namespace", x1.ns, x2.ns);

  if (x1.type_id && x2.type_id) {
    result.MaybeAddEdgeDiff("", (*this)(*x1.type_id, *x2.type_id));
  } else if (x1.type_id) {
    if (!options.ignore_symbol_type_presence_changes)
      result.AddEdgeDiff("", Removed(*x1.type_id));
  } else if (x2.type_id) {
    if (!options.ignore_symbol_type_presence_changes)
      result.AddEdgeDiff("", Added(*x2.type_id));
  } else {
    // both types missing, we have nothing to say
  }

  return result;
}

Result Compare::operator()(const Symbols& x1, const Symbols& x2) {
  Result result;
  result.diff_.holds_changes = true;

  // Group diffs into removed, added and changed symbols for readability.
  std::vector<Id> removed;
  std::vector<Id> added;
  std::vector<std::pair<Id, Id>> in_both;

  const auto& symbols1 = x1.symbols;
  const auto& symbols2 = x2.symbols;
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
    result.AddEdgeDiff("", Removed(symbol1));
  for (const auto symbol2 : added)
    result.AddEdgeDiff("", Added(symbol2));
  for (const auto& [symbol1, symbol2] : in_both)
    result.MaybeAddEdgeDiff("", (*this)(symbol1, symbol2));

  return result;
}

std::pair<Id, Qualifiers> ResolveQualifiers(const Graph& graph, Id id) {
  std::pair<Id, Qualifiers> result = {id, {}};
  ResolveQualifier resolve(graph, result.first, result.second);
  while (graph.Apply<bool>(resolve, result.first)) {
  }
  return result;
}

bool ResolveQualifier::operator()(const Array&) {
  // There should be no qualifiers here.
  qualifiers.clear();
  return false;
}

bool ResolveQualifier::operator()(const Function&) {
  // There should be no qualifiers here.
  qualifiers.clear();
  return false;
}

bool ResolveQualifier::operator()(const Qualified& x) {
  id = x.qualified_type_id;
  qualifiers.insert(x.qualifier);
  return true;
}

template <typename Node>
bool ResolveQualifier::operator()(const Node&) {
  return false;
}

std::pair<Id, std::vector<std::string>> ResolveTypedefs(
    const Graph& graph, Id id) {
  std::pair<Id, std::vector<std::string>> result = {id, {}};
  ResolveTypedef resolve(graph, result.first, result.second);
  while (graph.Apply<bool>(resolve, result.first)) {
  }
  return result;
}

bool ResolveTypedef::operator()(const Typedef& x) {
  id = x.referred_type_id;
  names.push_back(x.name);
  return true;
}

template <typename Node>
bool ResolveTypedef::operator()(const Node&) {
  return false;
}

std::string MatchingKey::operator()(Id id) {
  return graph.Apply<std::string>(*this, id);
}

std::string MatchingKey::operator()(const BaseClass& x) {
  return (*this)(x.type_id);
}

std::string MatchingKey::operator()(const Member& x) {
  if (!x.name.empty())
    return x.name;
  return (*this)(x.type_id);
}

std::string MatchingKey::operator()(const Method& x) {
  return x.name + ',' + x.mangled_name;
}

std::string MatchingKey::operator()(const StructUnion& x) {
  if (!x.name.empty())
    return x.name;
  if (x.definition) {
    const auto& members = x.definition->members;
    for (const auto& member : members) {
      const auto recursive = (*this)(member);
      if (!recursive.empty())
        return recursive + '+';
    }
  }
  return {};
}

template <typename Node>
std::string MatchingKey::operator()(const Node&) {
  return {};
}

}  // namespace stg
