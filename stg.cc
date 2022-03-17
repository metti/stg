// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2021 Google LLC
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

#include "stg.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <ostream>
#include <string_view>
#include <typeinfo>

#include "crc.h"
#include "error.h"
#include "order.h"

namespace stg {

bool Graph::Is(Id id) const {
  return types_[id.ix_] != nullptr;
}

Id Graph::Allocate() {
  const auto ix = types_.size();
  types_.push_back(nullptr);
  return Id(ix);
}

void Graph::Set(Id id, std::unique_ptr<Type> node) {
  Check(node != nullptr) << "node value not set";
  auto& reference = types_[id.ix_];
  Check(reference == nullptr) << "node value already set";
  reference = std::move(node);
}

Id Graph::Add(std::unique_ptr<Type> node) {
  auto id = Allocate();
  Set(id, std::move(node));
  return id;
}

static constexpr std::array<std::string_view, 6> kIntEncoding = {
    "boolean",
    "signed integer",
    "unsigned integer",
    "signed character",
    "unsigned character",
    "UTF",
};

Name Name::Add(Side side, Precedence precedence,
               const std::string& text) const {
  bool bracket = precedence < precedence_;
  std::ostringstream left;
  std::ostringstream right;

  // Bits on the left need (sometimes) to be separated by whitespace.
  left << left_;
  if (bracket)
    left << '(';
  else if (side == Side::LEFT)
    left << ' ';

  (side == Side::LEFT ? left : right) << text;

  // Bits on the right are arrays [] and functions () and need no whitespace.
  if (bracket)
    right << ')';
  right << right_;

  return Name{left.str(), precedence, right.str()};
}

Name Name::Qualify(const std::set<QualifierKind>& qualifiers) const {
  // this covers the case when bad qualifiers have been dropped
  if (qualifiers.empty())
    return *this;
  // add qualifiers to the left or right of the type stem
  std::ostringstream os;
  if (precedence_ == Precedence::NIL) {
    for (const auto& qualifier : qualifiers)
      os << qualifier << ' ';
    os << left_;
  } else if (precedence_ == Precedence::POINTER) {
    os << left_;
    for (const auto& qualifier : qualifiers)
      os << ' ' << qualifier;
  } else {
    // qualifiers do not apply to arrays, functions or names
    Die() << "qualifiers found for inappropriate node";
  }
  // qualifiers attach without affecting precedence
  return Name{os.str(), precedence_, right_};
}

std::ostream& Name::Print(std::ostream& os) const {
  return os << left_ << right_;
}

std::ostream& operator<<(std::ostream& os, const Name& name) {
  return name.Print(os);
}

const Name& Type::GetDescription(NameCache& names) const {
  // infinite recursion prevention - insert at most once
  static const Name black_hole{"#"};
  auto insertion = names.insert({this, black_hole});
  Name& cached = insertion.first->second;
  if (insertion.second)
    cached = MakeDescription(names);
  return cached;
}

std::string Type::GetResolvedDescription(NameCache& names) const {
  std::ostringstream os;
  os << '\'' << GetDescription(names) << '\'';
  return os.str();
}

std::string Typedef::GetResolvedDescription(NameCache& names) const {
  std::ostringstream os;
  std::vector<std::string> typedefs;
  const Type& type = ResolveTypedef(typedefs);
  for (auto td : typedefs)
    os << std::quoted(td, '\'') << " = ";
  os << '\'' << type.GetDescription(names) << '\'';
  return os.str();
}

bool PrintComparison(const Comparison& comparison, NameCache& names,
                     std::ostream& os) {
  const auto* node1 = comparison.first;
  const auto* node2 = comparison.second;
  if (!node2) {
    os << node1->GetKindDescription() << " '" << node1->GetDescription(names)
       << "' was removed\n";
    return true;
  }
  if (!node1) {
    os << node2->GetKindDescription() << " '" << node2->GetDescription(names)
       << "' was added\n";
    return true;
  }

  const auto description1 = node1->GetResolvedDescription(names);
  const auto description2 = node2->GetResolvedDescription(names);
  os << node1->GetKindDescription() << ' ';
  if (description1 == description2)
    os << description1 << " changed";
  else
    os << "changed from " << description1 << " to " << description2;
  return false;
}

static constexpr size_t INDENT_INCREMENT = 2;

void Print(const Comparison& comparison, const Outcomes& outcomes, Seen& seen,
           NameCache& names, std::ostream& os, size_t indent) {
  if (PrintComparison(comparison, names, os))
    return;

  const auto it = outcomes.find(comparison);
  Check(it != outcomes.end()) << "internal error: missing comparison";
  const auto& diff = it->second;

  const bool holds_changes = diff.holds_changes;
  std::pair<Seen::iterator, bool> insertion;

  if (holds_changes)
    insertion = seen.insert({comparison, false});

  if (holds_changes && !insertion.second) {
    if (!insertion.first->second)
      os << " (being reported)\n";
    else if (!diff.details.empty())
      os << " (already reported)\n";
    return;
  }

  os << '\n';
  Print(diff.details, outcomes, seen, names, os, indent + INDENT_INCREMENT);

  if (holds_changes)
    insertion.first->second = true;
}

void Print(const std::vector<DiffDetail>& details, const Outcomes& outcomes,
           Seen& seen, NameCache& names, std::ostream& os, size_t indent) {
  for (const auto& detail : details) {
    os << std::string(indent, ' ') << detail.text_;
    if (!detail.edge_) {
      os << '\n';
    } else {
      if (!detail.text_.empty())
        os << ' ';
      Print(*detail.edge_, outcomes, seen, names, os, indent);
    }
    // paragraph spacing
    if (!indent)
      os << '\n';
  }
}

// Print the subtree of a diff graph starting at a given node and stopping at
// nodes that can themselves hold diffs, queuing such nodes for subsequent
// printing. Optionally, avoid printing "uninteresting" nodes - those that have
// no diff and no path to a diff that does not pass through a node that can hold
// diffs. Return whether the diff node's tree was intrinisically interesting.
bool FlatPrint(const Comparison& comparison, const Outcomes& outcomes,
               std::unordered_set<Comparison, HashComparison>& seen,
               std::deque<Comparison>& todo, bool full, bool stop,
               NameCache& names, std::ostream& os, size_t indent) {
  // Nodes that represent additions or removal are always interesting and no
  // recursion is possible.
  if (PrintComparison(comparison, names, os))
    return true;

  // Look up the diff (including node and edge changes).
  const auto it = outcomes.find(comparison);
  Check(it != outcomes.end()) << "internal error: missing comparison";
  const auto& diff = it->second;

  os << '\n';

  // Check the stopping condition.
  if (diff.holds_changes && stop) {
    // If it's a new diff-holding node, queue it.
    if (seen.insert(comparison).second)
      todo.push_back(comparison);
    return false;
  }
  // The stop flag can only be false on a non-recursive call which should be for
  // a diff-holding node.
  if (!diff.holds_changes && !stop)
    Die() << "internal error: FlatPrint called on inappropriate node";

  // Indent before describing diff details.
  indent += INDENT_INCREMENT;
  bool interesting = diff.has_changes;
  for (const auto& detail : diff.details) {
    if (!detail.edge_) {
      os << std::string(indent, ' ') << detail.text_ << '\n';
      // Node changes may not be interesting, if we allow non-change diff
      // details at some point. Just trust the has_changes flag.
    } else {
      // Edge changes are interesting if the target diff node is.
      std::ostringstream sub_os;
      sub_os << std::string(indent, ' ') << detail.text_;
      if (!detail.text_.empty())
        sub_os << ' ';
      // Set the stop flag to prevent recursion past diff-holding nodes.
      bool sub_interesting = FlatPrint(*detail.edge_, outcomes, seen, todo,
                                       full, true, names, sub_os, indent);
      // If the sub-tree was interesting, add it.
      if (sub_interesting || full)
        os << sub_os.str();
      interesting |= sub_interesting;
    }
  }
  return interesting;
}

size_t VizId(std::unordered_map<Comparison, size_t, HashComparison>& ids,
             const Comparison& comparison) {
  return ids.insert({comparison, ids.size()}).first->second;
}

void VizPrint(const Comparison& comparison, const Outcomes& outcomes,
              std::unordered_set<Comparison, HashComparison>& seen,
              std::unordered_map<Comparison, size_t, HashComparison>& ids,
              NameCache& names, std::ostream& os) {
  if (!seen.insert(comparison).second)
    return;

  const auto node = VizId(ids, comparison);

  const auto* node1 = comparison.first;
  const auto* node2 = comparison.second;
  if (!node2) {
    os << "  \"" << node << "\" [color=red, label=\""
       << "removed(" << node1->GetDescription(names) << ")\"]\n";
    return;
  }
  if (!node1) {
    os << "  \"" << node << "\" [color=red, label=\""
       << "added(" << node2->GetDescription(names) << ")\"]\n";
    return;
  }

  const auto it = outcomes.find(comparison);
  Check(it != outcomes.end()) << "internal error: missing comparison";
  const auto& diff = it->second;
  const char* colour = diff.has_changes ? "color=red, " : "";
  const char* shape = diff.holds_changes ? "shape=rectangle, " : "";
  const auto description1 = node1->GetResolvedDescription(names);
  const auto description2 = node2->GetResolvedDescription(names);
  if (description1 == description2)
    os << "  \"" << node << "\" [" << colour << shape << "label=\""
       << description1 << "\"]\n";
  else
    os << "  \"" << node << "\" [" << colour << shape << "label=\""
       << description1 << " -> " << description2 << "\"]\n";

  size_t index = 0;
  for (const auto& detail : diff.details) {
    if (!detail.edge_) {
      // attribute change, create an implicit edge and node
      os << "  \"" << node << "\" -> \"" << node << ':' << index << "\"\n"
         << "  \"" << node << ':' << index << "\" [color=red, label=\""
         << detail.text_ << "\"]\n";
      ++index;
    } else {
      const auto& to = *detail.edge_;
      VizPrint(to, outcomes, seen, ids, names, os);
      os << "  \"" << node << "\" -> \"" << VizId(ids, to) << "\" [label=\""
         << detail.text_ << "\"]\n";
    }
  }
}

const Type& Type::GetType(Id id) const { return *(types_[id.ix_].get()); }

std::string QualifiersMessage(
    QualifierKind qualifier, const std::string& action) {
  std::ostringstream os;
  os << "qualifier " << qualifier << ' ' << action;
  return os.str();
}

Comparison Removed(State& state, const Type& node) {
  Comparison comparison{&node, nullptr};
  state.outcomes.insert({comparison, {}});
  return comparison;
}

Comparison Added(State& state, const Type& node) {
  Comparison comparison{nullptr, &node};
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
    State& state, const Type& node1, const Type& node2) {
  const Comparison comparison{&node1, &node2};

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

  Result result;

  std::set<QualifierKind> qualifiers1;
  std::set<QualifierKind> qualifiers2;
  const Type& unqualified1 = node1.ResolveQualifiers(qualifiers1);
  const Type& unqualified2 = node2.ResolveQualifiers(qualifiers2);
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
    const auto comp = Compare(state, unqualified1, unqualified2);
    result.MaybeAddEdgeDiff("underlying", comp);
  } else {
    std::vector<std::string> typedefs1;
    std::vector<std::string> typedefs2;
    const Type& resolved1 = unqualified1.ResolveTypedef(typedefs1);
    const Type& resolved2 = unqualified2.ResolveTypedef(typedefs2);
    if (&unqualified1 != &resolved1 || &unqualified2 != &resolved2) {
      // 3.2 Typedef difference.
      const auto comp = Compare(state, resolved1, resolved2);
      result.diff_.holds_changes = !typedefs1.empty() && !typedefs2.empty()
                                   && typedefs1[0] == typedefs2[0];
      result.MaybeAddEdgeDiff("resolved", comp);
    } else if (typeid(unqualified1) != typeid(unqualified2)) {
      // 4. Incomparable.
      result.equals_ = false;
      result.diff_.has_changes = true;
    } else {
      // 5. Actually compare with dynamic type dispatch.
      result = unqualified1.Equals(state, unqualified2);
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

Name Void::MakeDescription(NameCache&) const { return Name{"void"}; }

Name Variadic::MakeDescription(NameCache&) const { return Name{"..."}; }

Name Ptr::MakeDescription(NameCache& names) const {
  return GetType(GetPointeeTypeId())
      .GetDescription(names)
      .Add(Side::LEFT, Precedence::POINTER, "*");
}

Name Typedef::MakeDescription(NameCache&) const {
  return Name{GetName()};
}

Name Qualifier::MakeDescription(NameCache& names) const {
  std::set<QualifierKind> qualifiers;
  const Type& under = ResolveQualifiers(qualifiers);
  return under.GetDescription(names).Qualify(qualifiers);
}

Name Integer::MakeDescription(NameCache&) const {
  return Name{GetName()};
}

Name Array::MakeDescription(NameCache& names) const {
  std::ostringstream os;
  os << '[' << GetNumberOfElements() << ']';
  return GetType(GetElementTypeId())
      .GetDescription(names)
      .Add(Side::RIGHT, Precedence::ARRAY_FUNCTION, os.str());
}

Name Member::MakeDescription(NameCache& names) const {
  auto description = GetType(GetMemberType()).GetDescription(names);
  if (!name_.empty())
    description = description.Add(Side::LEFT, Precedence::ATOMIC, name_);
  if (bitsize_)
    description = description.Add(
        Side::RIGHT, Precedence::ATOMIC, " : " + std::to_string(bitsize_));
  return description;
}

Name StructUnion::MakeDescription(NameCache& names) const {
  std::ostringstream os;
  const auto& name = GetName();
  os << GetStructUnionKind() << ' ';
  if (!name.empty()) {
    os << GetName();
  } else if (const auto& definition = GetDefinition()) {
    os << "{ ";
    for (const auto& member : definition->members)
      os << GetType(member).GetDescription(names) << "; ";
    os << '}';
  }
  return Name{os.str()};
}

Name Enumeration::MakeDescription(NameCache&) const {
  std::ostringstream os;
  const auto& name = GetName();
  os << "enum ";
  if (!name.empty()) {
    os << GetName();
  } else if (const auto& definition = GetDefinition()) {
    os << "{ ";
    for (const auto& e : definition->enumerators)
      os << e.first << " = " << e.second << ", ";
    os << '}';
  }
  return Name{os.str()};
}

Name Function::MakeDescription(NameCache& names) const {
  std::ostringstream os;
  os << '(';
  bool sep = false;
  for (const auto& p : GetParameters()) {
    if (sep)
      os << ", ";
    else
      sep = true;
    const auto& arg_descr = GetType(p.typeId_).GetDescription(names);
    if (p.name_.empty())
      os << arg_descr;
    else
      os << arg_descr.Add(Side::LEFT, Precedence::ATOMIC, p.name_);
  }
  os << ')';
  return GetType(GetReturnTypeId())
      .GetDescription(names)
      .Add(Side::RIGHT, Precedence::ARRAY_FUNCTION, os.str());
}

Name ElfSymbol::MakeDescription(NameCache& names) const {
  const auto& name = symbol_->get_name();
  return type_id_
      ? GetType(*type_id_).GetDescription(names).Add(
          Side::LEFT, Precedence::ATOMIC, name)
      : Name{name};
}

Name Symbols::MakeDescription(NameCache&) const {
  return Name{"symbols"};
}

std::string Type::GetKindDescription() const { return "type"; }

std::string Member::GetKindDescription() const { return "member"; }

std::string ElfSymbol::GetKindDescription() const { return "symbol"; }

std::string Symbols::GetKindDescription() const { return "symbols"; }

Result Void::Equals(State&, const Type&) const { return {}; }

Result Variadic::Equals(State&, const Type&) const { return {}; }

Result Ptr::Equals(State& state, const Type& other) const {
  const auto& o = other.as<Ptr>();

  Result result;
  const auto ref_diff = Compare(state, GetType(GetPointeeTypeId()),
                                o.GetType(o.GetPointeeTypeId()));
  result.MaybeAddEdgeDiff("pointed-to", ref_diff);
  return result;
}

Result Typedef::Equals(State&, const Type&) const {
  // Compare will never attempt to directly compare Typedefs.
  Die() << "internal error: Typedef::Equals";
  __builtin_unreachable();
}

Result Qualifier::Equals(State&, const Type&) const {
  // Compare will never attempt to directly compare Qualifiers.
  Die() << "internal error: Qualifier::Equals";
  __builtin_unreachable();
}

Result Integer::Equals(State&, const Type& other) const {
  const auto& o = other.as<Integer>();

  Result result;
  result.MaybeAddNodeDiff("encoding", GetEncoding(), o.GetEncoding());
  result.MaybeAddNodeDiff("bit size", GetBitSize(), o.GetBitSize());
  if (GetBitSize() != GetByteSize() * 8 &&
      o.GetBitSize() != o.GetByteSize() * 8)
    result.MaybeAddNodeDiff("byte size", GetByteSize(), o.GetByteSize());
  return result;
}

Result Array::Equals(State& state, const Type& other) const {
  const auto& o = other.as<Array>();

  Result result;
  result.MaybeAddNodeDiff("number of elements",
                          GetNumberOfElements(), o.GetNumberOfElements());
  const auto element_type_diff =
      Compare(state, GetType(GetElementTypeId()),
              o.GetType(o.GetElementTypeId()));
  result.MaybeAddEdgeDiff("element", element_type_diff);
  return result;
}

static bool CompareDefined(bool defined1, bool defined2, Result& result) {
  if (defined1 && defined2)
    return true;
  if (defined1 != defined2) {
    std::ostringstream os;
    os << "was " << (defined1 ? "fully defined" : "only declared")
       << ", is now " << (defined2 ? "fully defined" : "only declared");
    result.AddNodeDiff(os.str());
  }
  return false;
}

static std::vector<std::pair<std::optional<size_t>, std::optional<size_t>>>
PairUp(const std::vector<std::pair<std::string, size_t>>& names1,
       const std::vector<std::pair<std::string, size_t>>& names2) {
  std::vector<std::pair<std::optional<size_t>, std::optional<size_t>>> pairs;
  pairs.reserve(std::max(names1.size(), names2.size()));
  auto it1 = names1.begin();
  auto it2 = names2.begin();
  const auto end1 = names1.end();
  const auto end2 = names2.end();
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
  Reorder(pairs);
  return pairs;
}

Result Member::Equals(State& state, const Type& other) const {
  const auto& o = other.as<Member>();

  Result result;
  result.MaybeAddNodeDiff("offset", offset_, o.offset_);
  result.MaybeAddNodeDiff("size", bitsize_, o.bitsize_);
  const auto sub_diff = Compare(state, GetType(typeId_), o.GetType(o.typeId_));
  result.MaybeAddEdgeDiff("", sub_diff);
  return result;
}

Result StructUnion::Equals(State& state, const Type& other) const {
  const auto& o = other.as<StructUnion>();

  Result result;
  // Compare two anonymous types recursively, not holding diffs.
  // Compare two identically named types recursively, holding diffs.
  // Everything else treated as distinct. No recursion.
  const auto kind1 = GetStructUnionKind();
  const auto kind2 = o.GetStructUnionKind();
  const auto& name1 = GetName();
  const auto& name2 = o.GetName();
  if (kind1 != kind2 || name1 != name2) {
    result.equals_ = false;
    result.diff_.has_changes = true;
    return result;
  }
  result.diff_.holds_changes = !name1.empty();
  const auto& definition1 = GetDefinition();
  const auto& definition2 = o.GetDefinition();
  if (!CompareDefined(definition1.has_value(), definition2.has_value(), result))
    return result;

  result.MaybeAddNodeDiff(
      "byte size", definition1->bytesize, definition2->bytesize);
  const auto& members1 = definition1->members;
  const auto& members2 = definition2->members;
  const auto names1 = GetMemberNames();
  const auto names2 = o.GetMemberNames();
  const auto pairs = PairUp(names1, names2);
  for (const auto& [index1, index2] : pairs) {
    if (index1 && !index2) {
      // removed
      const auto member1 = members1[*index1];
      result.AddEdgeDiff("", Removed(state, GetType(member1)));
    } else if (!index1 && index2) {
      // added
      const auto member2 = members2[*index2];
      result.AddEdgeDiff("", Added(state, o.GetType(member2)));
    } else {
      // in both
      const auto member1 = members1[*index1];
      const auto member2 = members2[*index2];
      result.MaybeAddEdgeDiff(
          "", Compare(state, GetType(member1), o.GetType(member2)));
    }
  }

  return result;
}

Result Enumeration::Equals(State&, const Type& other) const {
  const auto& o = other.as<Enumeration>();

  Result result;
  // Compare two anonymous types recursively, not holding diffs.
  // Compare two identically named types recursively, holding diffs.
  // Everything else treated as distinct. No recursion.
  const auto& name1 = GetName();
  const auto& name2 = o.GetName();
  if (name1 != name2) {
    result.equals_ = false;
    result.diff_.has_changes = true;
    return result;
  }
  result.diff_.holds_changes = !name1.empty();
  const auto& definition1 = GetDefinition();
  const auto& definition2 = o.GetDefinition();
  if (!CompareDefined(definition1.has_value(), definition2.has_value(), result))
    return result;
  result.MaybeAddNodeDiff(
      "byte size", definition1->bytesize, definition2->bytesize);

  const auto enums1 = definition1->enumerators;
  const auto enums2 = definition2->enumerators;
  const auto names1 = GetEnumNames();
  const auto names2 = o.GetEnumNames();
  const auto pairs = PairUp(names1, names2);
  for (const auto& [index1, index2] : pairs) {
    if (index1 && !index2) {
      // removed
      const auto& enum1 = enums1[*index1];
      std::ostringstream os;
      os << "enumerator " << std::quoted(enum1.first, '\'')
         << " (" << enum1.second << ") was removed";
      result.AddNodeDiff(os.str());
    } else if (!index1 && index2) {
      // added
      const auto& enum2 = enums2[*index2];
      std::ostringstream os;
      os << "enumerator " << std::quoted(enum2.first, '\'')
         << " (" << enum2.second << ") was added";
      result.AddNodeDiff(os.str());
    } else {
      // in both
      const auto& enum1 = enums1[*index1];
      const auto& enum2 = enums2[*index2];
      result.MaybeAddNodeDiff(
          [&](std::ostream& os) {
            os << "enumerator " << std::quoted(enum1.first, '\'') << " value";
          },
          enum1.second, enum2.second);
    }
  }

  return result;
}

Result Function::Equals(State& state, const Type& other) const {
  const auto& o = other.as<Function>();

  Result result;
  const auto return_type_diff = Compare(state, GetType(GetReturnTypeId()),
                                        o.GetType(o.GetReturnTypeId()));
  result.MaybeAddEdgeDiff("return", return_type_diff);

  const auto& parameters1 = GetParameters();
  const auto& parameters2 = o.GetParameters();
  size_t min = std::min(parameters1.size(), parameters2.size());
  for (size_t i = 0; i < min; ++i) {
    const auto& p1 = parameters1.at(i);
    const auto& p2 = parameters2.at(i);
    const auto sub_diff =
        Compare(state, GetType(p1.typeId_), o.GetType(p2.typeId_));
    result.MaybeAddEdgeDiff(
        [&](std::ostream& os) {
          os << "parameter " << i + 1;
          const auto& n1 = p1.name_;
          const auto& n2 = p2.name_;
          if (n1 == n2 && !n1.empty()) {
            os << " (" << std::quoted(n1, '\'') << ")";
          } else if (n1 != n2) {
            os << " (";
            if (!n1.empty())
              os << "was " << std::quoted(n1, '\'');
            if (!n1.empty() && !n2.empty())
              os << ", ";
            if (!n2.empty())
              os << "now " << std::quoted(n2, '\'');
            os << ")";
          }
        },
        sub_diff);
  }

  bool added = parameters1.size() < parameters2.size();
  const auto& which = added ? o : *this;
  const auto& parameters = which.GetParameters();
  for (size_t i = min; i < parameters.size(); ++i) {
    const auto& parameter = parameters.at(i);
    std::ostringstream os;
    os << "parameter " << i + 1;
    if (!parameter.name_.empty())
      os << " (" << std::quoted(parameter.name_, '\'') << ")";
    os << " of";
    const auto& parameter_type = which.GetType(parameter.typeId_);
    auto diff =
        added ? Added(state, parameter_type) : Removed(state, parameter_type);
    result.AddEdgeDiff(os.str(), diff);
  }

  return result;
}

Result ElfSymbol::Equals(State& state, const Type& other) const {
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
  // Symbol namespaces - not yet supported by symtab_reader

  const auto& s1 = *symbol_;
  const auto& s2 = *o.symbol_;

  Result result;
  result.MaybeAddNodeDiff("name", s1.get_name(), s2.get_name());

  // Abigail ELF symbol version encapsulates both a version string and a default
  // flag but only the former is used in its equality operator! Abigail also
  // conflates no version with an empty version (though the latter may be
  // illegal).
  const auto version1 = s1.get_version();
  const auto version2 = s2.get_version();
  result.MaybeAddNodeDiff("version", version1.str(), version2.str());
  result.MaybeAddNodeDiff(
      "default version", version1.is_default(), version2.is_default());

  result.MaybeAddNodeDiff("defined", s1.is_defined(), s2.is_defined());
  result.MaybeAddNodeDiff("symbol type", s1.get_type(), s2.get_type());
  result.MaybeAddNodeDiff("binding", s1.get_binding(), s2.get_binding());
  result.MaybeAddNodeDiff(
      "visibility", s1.get_visibility(), s2.get_visibility());

  result.MaybeAddNodeDiff("CRC", CRC{s1.get_crc()}, CRC{s2.get_crc()});

  if (type_id_ && o.type_id_) {
    const auto type_diff =
        Compare(state, GetType(*type_id_), o.GetType(*o.type_id_));
    result.MaybeAddEdgeDiff("", type_diff);
  } else if (type_id_) {
    result.AddEdgeDiff("", Removed(state, GetType(*type_id_)));
  } else if (o.type_id_) {
    result.AddEdgeDiff("", Added(state, o.GetType(*o.type_id_)));
  } else {
    // both types missing, we have nothing to say
  }

  return result;
}

Result Symbols::Equals(State& state, const Type& other) const {
  const auto& o = other.as<Symbols>();

  Result result;
  result.diff_.holds_changes = true;

  // Group diffs into removed, added and changed symbols for readability.
  std::vector<Id> removed;
  std::vector<Id> added;
  std::vector<std::pair<Id, Id>> in_both;

  const auto& symbols1 = symbols_;
  const auto& symbols2 = o.symbols_;
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
    result.AddEdgeDiff("", Removed(state, GetType(symbol1)));
  for (const auto symbol2 : added)
    result.AddEdgeDiff("", Added(state, o.GetType(symbol2)));
  for (const auto [symbol1, symbol2] : in_both)
    result.MaybeAddEdgeDiff(
        "", Compare(state, GetType(symbol1), o.GetType(symbol2)));

  return result;
}

const Type& Type::ResolveQualifiers(std::set<QualifierKind>&) const {
  return *this;
}

const Type& Array::ResolveQualifiers(
    std::set<QualifierKind>& qualifiers) const {
  // There should be no qualifiers here.
  qualifiers.clear();
  return *this;
}

const Type& Function::ResolveQualifiers(
    std::set<QualifierKind>& qualifiers) const {
  // There should be no qualifiers here.
  qualifiers.clear();
  return *this;
}

const Type& Qualifier::ResolveQualifiers(
    std::set<QualifierKind>& qualifiers) const {
  qualifiers.insert(GetQualifierKind());
  return GetType(GetQualifiedTypeId()).ResolveQualifiers(qualifiers);
}

const Type& Type::ResolveTypedef(std::vector<std::string>&) const {
  return *this;
}

const Type& Typedef::ResolveTypedef(std::vector<std::string>& typedefs) const {
  typedefs.push_back(GetName());
  return GetType(GetReferredTypeId()).ResolveTypedef(typedefs);
}

std::string Type::GetFirstName() const { return {}; }

std::string Member::GetFirstName() const {
  if (!name_.empty())
    return name_;
  const auto& type = GetType(typeId_);
  return type.GetFirstName();
}

std::string StructUnion::GetFirstName() const {
  const auto& name = GetName();
  if (!name.empty())
    return name;
  if (const auto& definition = GetDefinition()) {
    const auto& members = definition->members;
    for (const auto& member : members) {
      const auto recursive = GetType(member).GetFirstName();
      if (!recursive.empty())
        return recursive;
    }
  }
  return {};
}

std::vector<std::pair<std::string, size_t>> StructUnion::GetMemberNames()
    const {
  std::vector<std::pair<std::string, size_t>> names;
  if (const auto& definition = GetDefinition()) {
    const auto& members = definition->members;
    const auto size = members.size();
    names.reserve(size);
    size_t anonymous_ix = 0;
    for (size_t ix = 0; ix < size; ++ix) {
      const auto& member = GetType(members[ix]);
      auto key = member.GetFirstName();
      if (key.empty())
        key = "#anon#" + std::to_string(anonymous_ix++);
      names.push_back({key, ix});
    }
    std::stable_sort(names.begin(), names.end());
  }
  return names;
}

std::vector<std::pair<std::string, size_t>> Enumeration::GetEnumNames() const {
  std::vector<std::pair<std::string, size_t>> names;
  if (const auto& definition = GetDefinition()) {
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

std::ostream& operator<<(std::ostream& os, StructUnionKind kind) {
  switch (kind) {
    case StructUnionKind::STRUCT:
      os << "struct";
      break;
    case StructUnionKind::UNION:
      os << "union";
      break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, QualifierKind kind) {
  switch (kind) {
    case QualifierKind::CONST:
      os << "const";
      break;
    case QualifierKind::VOLATILE:
      os << "volatile";
      break;
    case QualifierKind::RESTRICT:
      os << "restrict";
      break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, Integer::Encoding encoding) {
  auto ix = static_cast<size_t>(encoding);
  return os << (ix < kIntEncoding.size() ? kIntEncoding[ix] : "(unknown)");
}

}  // namespace stg
