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

template <typename Kind, typename... Args>
    std::unique_ptr<Node> Make(Args&&... args) {
  return std::make_unique<Kind>(std::forward<Args>(args)...);
}

// Concrete graph type.
class Graph {
 public:
  const Node& Get(Id id) const;

  bool Is(Id) const;
  Id Allocate();
  void Set(Id id, std::unique_ptr<Node> node);
  Id Add(std::unique_ptr<Node> node);

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

}  // namespace stg

#endif  // STG_STG_H_
