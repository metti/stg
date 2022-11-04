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

#ifndef STG_STG_H_
#define STG_STG_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "crc.h"
#include "id.h"
#include "scc.h"

namespace stg {

struct Node;

// Concrete graph type.
class Graph {
 public:
  const Node& Get(Id id) const {
    return *nodes_[id.ix_].get();
  }

  bool Is(Id id) const {
    return nodes_[id.ix_] != nullptr;
  }

  Id Allocate() {
    const auto ix = nodes_.size();
    nodes_.push_back(nullptr);
    return Id(ix);
  }

  template <typename Node, typename... Args>
  void Set(Id id, Args&&... args) {
    auto& reference = nodes_[id.ix_];
    Check(reference == nullptr) << "node value already set";
    reference = std::make_unique<Node>(std::forward<Args>(args)...);
  }

  template <typename Node, typename... Args>
  Id Add(Args&&... args) {
    auto id = Allocate();
    Set<Node>(id, std::forward<Args>(args)...);
    return id;
  }

  template <typename Result, typename FunctionObject>
  Result Apply(FunctionObject& function, Id id) const;

  template <typename Result, typename FunctionObject>
  Result Apply(FunctionObject& function, Id id1, Id id2) const;

 private:
  std::vector<std::unique_ptr<Node>> nodes_;
};

enum class Qualifier { CONST, VOLATILE, RESTRICT };
using Qualifiers = std::set<Qualifier>;

std::ostream& operator<<(std::ostream& os, Qualifier qualifier);

using Comparison = std::pair<std::optional<Id>, std::optional<Id>>;

enum class Precedence { NIL, POINTER, ARRAY_FUNCTION, ATOMIC };
enum class Side { LEFT, RIGHT };

class Name {
 public:
  explicit Name(const std::string& name)
      : left_(name), precedence_(Precedence::NIL), right_() {}
  Name(const std::string& left, Precedence precedence, const std::string& right)
      : left_(left), precedence_(precedence), right_(right) {}
  Name Add(Side side, Precedence precedence, const std::string& text) const;
  Name Qualify(Qualifier qualifier) const;
  std::ostream& Print(std::ostream& os) const;

 private:
  std::string left_;
  Precedence precedence_;
  std::string right_;
};

std::ostream& operator<<(std::ostream& os, const Name& name);

using NameCache = std::unordered_map<Id, Name>;

Id ResolveQualifiers(const Graph& graph, Id id, Qualifiers& qualifiers);
Id ResolveTypedefs(
    const Graph& graph, Id id, std::vector<std::string>& typedefs);

const Name& GetDescription(const Graph& graph, NameCache& names, Id node);
std::string GetResolvedDescription(const Graph& graph, NameCache& names, Id id);

struct DiffDetail {
  DiffDetail(const std::string& text, const std::optional<Comparison>& edge)
      : text_(text), edge_(edge) {}
  std::string text_;
  std::optional<Comparison> edge_;
};

struct Diff {
  // This diff node corresponds to an entity that is reportable, if it or any of
  // its children (excluding reportable ones) has changed.
  bool holds_changes = false;
  // This diff node contains a local (non-recursive) change.
  bool has_changes = false;
  std::vector<DiffDetail> details;

  void Add(const std::string& text,
           const std::optional<Comparison>& comparison) {
    details.emplace_back(text, comparison);
  }
};

struct Result {
  // Used when two nodes cannot be meaningfully compared.
  Result& MarkIncomparable() {
    equals_ = false;
    diff_.has_changes = true;
    return *this;
  }

  // Used when a node attribute has changed.
  void AddNodeDiff(const std::string& text) {
    equals_ = false;
    diff_.has_changes = true;
    diff_.Add(text, {});
  }

  // Used when a node attribute may have changed.
  template <typename T>
  void MaybeAddNodeDiff(
      const std::string& text, const T& before, const T& after) {
    if (before != after) {
      std::ostringstream os;
      os << text << " changed from " << before << " to " << after;
      AddNodeDiff(os.str());
    }
  }

  // Used when a node attribute may have changed, lazy version.
  template <typename T>
  void MaybeAddNodeDiff(std::function<void(std::ostream&)> text,
                        const T& before, const T& after) {
    if (before != after) {
      std::ostringstream os;
      text(os);
      os << " changed from " << before << " to " << after;
      AddNodeDiff(os.str());
    }
  }

  // Used when node attributes are optional values.
  template <typename T>
  void MaybeAddNodeDiff(const std::string& text, const std::optional<T>& before,
                        const std::optional<T>& after) {
    if (before && after) {
      MaybeAddNodeDiff(text, *before, *after);
    } else if (before) {
      std::ostringstream os;
      os << text << *before << " was removed";
      AddNodeDiff(os.str());
    } else if (after) {
      std::ostringstream os;
      os << text << *after << " was added";
      AddNodeDiff(os.str());
    }
  }

  // Used when an edge has been removed or added.
  void AddEdgeDiff(const std::string& text, const Comparison& comparison) {
    equals_ = false;
    diff_.Add(text, {comparison});
  }

  // Used when an edge to a possible comparison is present.
  void MaybeAddEdgeDiff(const std::string& text,
                        const std::pair<bool, std::optional<Comparison>>& p) {
    equals_ &= p.first;
    const auto& comparison = p.second;
    if (comparison)
      diff_.Add(text, comparison);
  }

  // Used when an edge to a possible comparison is present, lazy version.
  void MaybeAddEdgeDiff(std::function<void(std::ostream&)> text,
                        const std::pair<bool, std::optional<Comparison>>& p) {
    equals_ &= p.first;
    const auto& comparison = p.second;
    if (comparison) {
      std::ostringstream os;
      text(os);
      diff_.Add(os.str(), comparison);
    }
  }

  bool equals_ = true;
  Diff diff_;
};

struct HashComparison {
  size_t operator()(const Comparison& comparison) const {
    size_t seed = 0;
    std::hash<std::optional<Id>> h;
    combine_hash(seed, h(comparison.first));
    combine_hash(seed, h(comparison.second));
    return seed;
  }
  static void combine_hash(size_t& seed, size_t hash) {
    seed ^= hash + 0x9e3779b97f4a7c15 + (seed << 12) + (seed >> 4);
  }
};

using Outcomes = std::unordered_map<Comparison, Diff, HashComparison>;

struct CompareOptions {
  bool ignore_symbol_type_presence_changes = false;
  bool ignore_type_declaration_status_changes = false;
};

struct State {
  State(const Graph& g, const CompareOptions& o) : graph(g), options(o) {}
  const Graph& graph;
  const CompareOptions options;
  std::unordered_map<Comparison, bool, HashComparison> known;
  Outcomes outcomes;
  Outcomes provisional;
  SCC<Comparison, HashComparison> scc;
};

struct Node {
  Node() = default;
  Node(const Node&) = delete;
  Node(Node&&) = default;
  virtual ~Node() = default;

  // as<Target>() provides a method to delegate downcasting to the base class,
  // instead of needing to use dynamic_cast in a local context. If the type is
  // not correct, an exception will be thrown.
  template <typename Target>
  const Target& as() const {
    static_assert(std::is_convertible<Target*, Node*>::value,
                  "Target must publically inherit Node");
    return dynamic_cast<const Target&>(*this);
  }
  // Separate qualifiers from underlying type.
  //
  // The caller must always be prepared to receive a different type as
  // qualifiers are sometimes discarded.
  virtual std::optional<Id> ResolveQualifier(Qualifiers& qualifiers) const;
  virtual std::optional<Id> ResolveTypedef(
      std::vector<std::string>& typedefs) const;
  virtual std::string MatchingKey(const Graph& graph) const;

  virtual Name MakeDescription(const Graph& graph, NameCache& names) const = 0;
  virtual std::string ExtraDescription() const;
  virtual std::string GetKindDescription() const;

  virtual Result Equals(State& state, const Node& other) const = 0;
};

Comparison Removed(State& state, Id node);
Comparison Added(State& state, Id node);
std::pair<bool, std::optional<Comparison>> Compare(
    State& state, Id node1, const Id node2);

struct Void : Node {
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
};

struct Variadic : Node {
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
};

struct PointerReference : Node {
  enum class Kind {
    POINTER,
    LVALUE_REFERENCE,
    RVALUE_REFERENCE,
  };
  PointerReference(Kind kind, Id pointee_type_id)
      : kind(kind), pointee_type_id(pointee_type_id) {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;

  const Kind kind;
  const Id pointee_type_id;
};

std::ostream& operator<<(std::ostream& os, PointerReference::Kind kind);

struct Typedef : Node {
  Typedef(const std::string& name, Id referred_type_id)
      : name(name), referred_type_id(referred_type_id) {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::optional<Id> ResolveTypedef(
      std::vector<std::string>& typedefs) const final;

  const std::string name;
  const Id referred_type_id;
};

struct Qualified : Node {
  Qualified(Qualifier qualifier, Id qualified_type_id)
      : qualifier(qualifier), qualified_type_id(qualified_type_id) {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::optional<Id> ResolveQualifier(Qualifiers& qualifiers) const final;

  const Qualifier qualifier;
  const Id qualified_type_id;
};

struct Primitive : Node {
  enum class Encoding {
    BOOLEAN,
    SIGNED_INTEGER,
    UNSIGNED_INTEGER,
    SIGNED_CHARACTER,
    UNSIGNED_CHARACTER,
    REAL_NUMBER,
    COMPLEX_NUMBER,
    UTF,
  };
  Primitive(const std::string& name, std::optional<Encoding> encoding,
            uint32_t bitsize, uint32_t bytesize)
      : name(name), encoding(encoding), bitsize(bitsize), bytesize(bytesize) {}

  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;

  const std::string name;
  const std::optional<Encoding> encoding;
  // bitsize gives the semantics of the field. bytesize gives the
  // storage size, and is equal to bitsize / 8 rounded up.
  const uint32_t bitsize;
  const uint32_t bytesize;
};

struct Array : Node {
  Array(uint64_t number_of_elements, Id element_type_id)
      : number_of_elements(number_of_elements),
        element_type_id(element_type_id)  {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::optional<Id> ResolveQualifier(Qualifiers& qualifiers) const final;

  const uint64_t number_of_elements;
  const Id element_type_id;
};

struct BaseClass : Node {
  enum class Inheritance { NON_VIRTUAL, VIRTUAL };
  BaseClass(Id type_id, uint64_t offset, Inheritance inheritance)
      : type_id(type_id), offset(offset), inheritance(inheritance) {}
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::string MatchingKey(const Graph& graph) const final;

  const Id type_id;
  const uint64_t offset;
  const Inheritance inheritance;
};

std::ostream& operator<<(std::ostream& os, BaseClass::Inheritance inheritance);

struct Member : Node {
  Member(const std::string& name, Id type_id, uint64_t offset, uint64_t bitsize)
      : name(name), type_id(type_id), offset(offset), bitsize(bitsize) {}
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::string MatchingKey(const Graph& graph) const final;

  const std::string name;
  const Id type_id;
  const uint64_t offset;
  const uint64_t bitsize;
};

struct Method : Node {
  enum class Kind { NON_VIRTUAL, STATIC, VIRTUAL };
  Method(const std::string& mangled_name, const std::string& name, Kind kind,
         const std::optional<uint64_t> vtable_offset, Id type_id)
      : mangled_name(mangled_name), name(name), kind(kind),
        vtable_offset(vtable_offset), type_id(type_id) {}
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::string MatchingKey(const Graph& graph) const final;

  const std::string mangled_name;
  const std::string name;
  const Kind kind;
  const std::optional<uint64_t> vtable_offset;
  const Id type_id;
};

std::ostream& operator<<(std::ostream& os, Method::Kind kind);

struct StructUnion : Node {
  enum class Kind { CLASS, STRUCT, UNION };
  struct Definition {
    const uint64_t bytesize;
    const std::vector<Id> base_classes;
    const std::vector<Id> methods;
    const std::vector<Id> members;
  };
  StructUnion(Kind kind, const std::string& name) : kind(kind), name(name) {}
  StructUnion(Kind kind, const std::string& name, uint64_t bytesize,
              const std::vector<Id>& base_classes,
              const std::vector<Id>& methods, const std::vector<Id>& members)
      : kind(kind), name(name),
        definition({bytesize, base_classes, methods, members}) {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::string MatchingKey(const Graph& graph) const final;

  const Kind kind;
  const std::string name;
  const std::optional<Definition> definition;
};

std::ostream& operator<<(std::ostream& os, StructUnion::Kind kind);

struct Enumeration : Node {
  using Enumerators = std::vector<std::pair<std::string, int64_t>>;
  struct Definition {
    const uint32_t bytesize;
    const Enumerators enumerators;
  };
  Enumeration(const std::string& name) : name(name) {}
  Enumeration(const std::string& name, uint32_t bytesize,
              const Enumerators& enumerators)
      : name(name), definition({bytesize, enumerators}) {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;

  std::vector<std::pair<std::string, size_t>> GetEnumNames() const;
  const std::string name;
  const std::optional<Definition> definition;
};

struct Function : Node {
  Function(Id return_type_id, const std::vector<Id>& parameters)
      : return_type_id(return_type_id), parameters(parameters) {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;
  std::optional<Id> ResolveQualifier(Qualifiers& qualifiers) const final;

  const Id return_type_id;
  const std::vector<Id> parameters;
};

struct ElfSymbol : Node {
  enum class SymbolType { OBJECT, FUNCTION, COMMON, TLS };
  enum class Binding { GLOBAL, LOCAL, WEAK, GNU_UNIQUE };
  enum class Visibility { DEFAULT, PROTECTED, HIDDEN, INTERNAL };
  struct VersionInfo {
    // TODO: auto operator<=>(const VersionInfo&) const = default;
    bool operator==(const VersionInfo& other) const {
      return is_default == other.is_default && name == other.name;
    }
    bool is_default;
    std::string name;
  };
  ElfSymbol(const std::string& symbol_name,
            std::optional<VersionInfo> version_info,
            bool is_defined,
            SymbolType symbol_type,
            Binding binding,
            Visibility visibility,
            std::optional<CRC> crc,
            std::optional<std::string> ns,
            std::optional<Id> type_id,
            const std::optional<std::string>& full_name)
      : symbol_name(symbol_name),
        version_info(version_info),
        is_defined(is_defined),
        symbol_type(symbol_type),
        binding(binding),
        visibility(visibility),
        crc(crc),
        ns(ns),
        type_id(type_id),
        full_name(full_name) {}
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  std::string ExtraDescription() const final;
  Result Equals(State& state, const Node& other) const final;

  const std::string symbol_name;
  const std::optional<VersionInfo> version_info;
  const bool is_defined;
  const SymbolType symbol_type;
  const Binding binding;
  const Visibility visibility;
  const std::optional<CRC> crc;
  const std::optional<std::string> ns;
  const std::optional<Id> type_id;
  const std::optional<std::string> full_name;
};

std::ostream& operator<<(std::ostream& os, ElfSymbol::SymbolType);
std::ostream& operator<<(std::ostream& os, ElfSymbol::Binding);
std::ostream& operator<<(std::ostream& os, ElfSymbol::Visibility);

std::string VersionInfoToString(const ElfSymbol::VersionInfo& version_info);

struct Symbols : Node {
  Symbols(const std::map<std::string, Id>& symbols) : symbols(symbols) {}
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Node& other) const final;

  const std::map<std::string, Id> symbols;
};

std::ostream& operator<<(std::ostream& os, Primitive::Encoding encoding);

template <typename Result, typename FunctionObject>
Result Graph::Apply(FunctionObject& function, Id id) const {
  const Node& node = Get(id);
  const auto& type_id = typeid(node);
  if (type_id == typeid(Void)) {
    return function(static_cast<const Void&>(node));
  }
  if (type_id == typeid(Variadic)) {
    return function(static_cast<const Variadic&>(node));
  }
  if (type_id == typeid(PointerReference)) {
    return function(static_cast<const PointerReference&>(node));
  }
  if (type_id == typeid(Typedef)) {
    return function(static_cast<const Typedef&>(node));
  }
  if (type_id == typeid(Qualified)) {
    return function(static_cast<const Qualified&>(node));
  }
  if (type_id == typeid(Primitive)) {
    return function(static_cast<const Primitive&>(node));
  }
  if (type_id == typeid(Array)) {
    return function(static_cast<const Array&>(node));
  }
  if (type_id == typeid(BaseClass)) {
    return function(static_cast<const BaseClass&>(node));
  }
  if (type_id == typeid(Member)) {
    return function(static_cast<const Member&>(node));
  }
  if (type_id == typeid(Method)) {
    return function(static_cast<const Method&>(node));
  }
  if (type_id == typeid(StructUnion)) {
    return function(static_cast<const StructUnion&>(node));
  }
  if (type_id == typeid(Enumeration)) {
    return function(static_cast<const Enumeration&>(node));
  }
  if (type_id == typeid(Function)) {
    return function(static_cast<const Function&>(node));
  }
  if (type_id == typeid(ElfSymbol)) {
    return function(static_cast<const ElfSymbol&>(node));
  }
  if (type_id == typeid(Symbols)) {
    return function(static_cast<const Symbols&>(node));
  }
  Die() << "unknown node type " << type_id.name();
}

template <typename Result, typename FunctionObject>
Result Graph::Apply(FunctionObject& function, Id id1, Id id2) const {
  const Node& node1 = Get(id1);
  const Node& node2 = Get(id2);
  const auto& type_id1 = typeid(node1);
  const auto& type_id2 = typeid(node2);
  if (type_id1 != type_id2) {
    return function.Mismatch();
  }
  if (type_id1 == typeid(Void)) {
    return function(static_cast<const Void&>(node1),
                    static_cast<const Void&>(node2));
  }
  if (type_id1 == typeid(Variadic)) {
    return function(static_cast<const Variadic&>(node1),
                    static_cast<const Variadic&>(node2));
  }
  if (type_id1 == typeid(PointerReference)) {
    return function(static_cast<const PointerReference&>(node1),
                    static_cast<const PointerReference&>(node2));
  }
  if (type_id1 == typeid(Typedef)) {
    return function(static_cast<const Typedef&>(node1),
                    static_cast<const Typedef&>(node2));
  }
  if (type_id1 == typeid(Qualified)) {
    return function(static_cast<const Qualified&>(node1),
                    static_cast<const Qualified&>(node2));
  }
  if (type_id1 == typeid(Primitive)) {
    return function(static_cast<const Primitive&>(node1),
                    static_cast<const Primitive&>(node2));
  }
  if (type_id1 == typeid(Array)) {
    return function(static_cast<const Array&>(node1),
                    static_cast<const Array&>(node2));
  }
  if (type_id1 == typeid(BaseClass)) {
    return function(static_cast<const BaseClass&>(node1),
                    static_cast<const BaseClass&>(node2));
  }
  if (type_id1 == typeid(Member)) {
    return function(static_cast<const Member&>(node1),
                    static_cast<const Member&>(node2));
  }
  if (type_id1 == typeid(Method)) {
    return function(static_cast<const Method&>(node1),
                    static_cast<const Method&>(node2));
  }
  if (type_id1 == typeid(StructUnion)) {
    return function(static_cast<const StructUnion&>(node1),
                    static_cast<const StructUnion&>(node2));
  }
  if (type_id1 == typeid(Enumeration)) {
    return function(static_cast<const Enumeration&>(node1),
                    static_cast<const Enumeration&>(node2));
  }
  if (type_id1 == typeid(Function)) {
    return function(static_cast<const Function&>(node1),
                    static_cast<const Function&>(node2));
  }
  if (type_id1 == typeid(ElfSymbol)) {
    return function(static_cast<const ElfSymbol&>(node1),
                    static_cast<const ElfSymbol&>(node2));
  }
  if (type_id1 == typeid(Symbols)) {
    return function(static_cast<const Symbols&>(node1),
                    static_cast<const Symbols&>(node2));
  }
  Die() << "unknown node type " << type_id1.name();
}

}  // namespace stg

#endif  // STG_STG_H_
