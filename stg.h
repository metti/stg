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

#include <abg-ir.h>  // for ELF symbol bits
#include "id.h"
#include "scc.h"

namespace stg {

class Type;

template <typename Kind, typename... Args>
    std::unique_ptr<Type> Make(Args&&... args) {
  return std::make_unique<Kind>(std::forward<Args>(args)...);
}

// Concrete graph type.
class Graph {
 public:
  const Type& Get(Id id) const;

  bool Is(Id) const;
  Id Allocate();
  void Set(Id id, std::unique_ptr<Type> node);
  Id Add(std::unique_ptr<Type> node);

 private:
  std::vector<std::unique_ptr<Type>> types_;
};

// A Parameter refers to a variable declared in the function declaration, used
// in the context of Function.
struct Parameter {
  std::string name_;
  Id typeId_;
};

enum class StructUnionKind { STRUCT, UNION };
enum class QualifierKind { CONST, VOLATILE, RESTRICT };

std::ostream& operator<<(std::ostream& os, StructUnionKind kind);
std::ostream& operator<<(std::ostream& os, QualifierKind kind);

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
  Name Qualify(const std::set<QualifierKind>& qualifiers) const;
  std::ostream& Print(std::ostream& os) const;

 private:
  std::string left_;
  Precedence precedence_;
  std::string right_;
};

std::ostream& operator<<(std::ostream& os, const Name& name);

using NameCache = std::unordered_map<Id, Name>;

Id ResolveQualifiers(
    const Graph& graph, Id id, std::set<QualifierKind>& qualifiers);
Id ResolveTypedefs(
    const Graph& graph, Id id, std::vector<std::string>& typedefs);
std::string GetFirstName(const Graph& graph, Id id);

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

struct State {
  explicit State(const Graph& g) : graph(g) {}
  const Graph& graph;
  std::unordered_map<Comparison, bool, HashComparison> known;
  Outcomes outcomes;
  Outcomes provisional;
  SCC<Comparison, HashComparison> scc;
};

class Type {
 public:
  virtual ~Type() = default;

  // as<Type>() provides a method to defer downcasting to the base class,
  // instead of needing to use dynamic_cast in a local context. If the type is
  // not correct, an exception will be thrown.
  template <typename Target>
  const Target& as() const {
    static_assert(std::is_convertible<Target*, Type*>::value,
                  "Target must publically inherit Type");
    return dynamic_cast<const Target&>(*this);
  }
  // Separate qualifiers from underlying type.
  //
  // The caller must always be prepared to receive a different type as
  // qualifiers are sometimes discarded.
  virtual std::optional<Id> ResolveQualifier(
      std::set<QualifierKind>& qualifiers) const;
  virtual std::optional<Id> ResolveTypedef(
      std::vector<std::string>& typedefs) const;
  virtual std::string FirstName(const Graph& graph) const;

  virtual Name MakeDescription(const Graph& graph, NameCache& names) const = 0;
  virtual std::string GetKindDescription() const;

  virtual Result Equals(State& state, const Type& other) const = 0;
};

Comparison Removed(State& state, Id node);
Comparison Added(State& state, Id node);
std::pair<bool, std::optional<Comparison>> Compare(
    State& state, Id node1, const Id node2);

class Void : public Type {
 public:
  Void() {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
};

class Variadic : public Type {
 public:
  Variadic() {}
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
};

class Ptr : public Type {
 public:
  Ptr(Id pointeeTypeId)
      : pointeeTypeId_(pointeeTypeId) {}
  Id GetPointeeTypeId() const { return pointeeTypeId_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;

 private:
  const Id pointeeTypeId_;
};

class Typedef : public Type {
 public:
  Typedef(const std::string& name, Id referredTypeId)
      : name_(name),
        referredTypeId_(referredTypeId) {}
  const std::string& GetName() const { return name_; }
  Id GetReferredTypeId() const { return referredTypeId_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
  std::optional<Id> ResolveTypedef(
      std::vector<std::string>& typedefs) const final;

 private:
  const std::string name_;
  const Id referredTypeId_;
};

class Qualifier : public Type {
 public:
  Qualifier(QualifierKind qualifierKind, Id qualifiedTypeId)
      : qualifierKind_(qualifierKind),
        qualifiedTypeId_(qualifiedTypeId) {}
  QualifierKind GetQualifierKind() const { return qualifierKind_; }
  Id GetQualifiedTypeId() const { return qualifiedTypeId_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
  std::optional<Id> ResolveQualifier(
      std::set<QualifierKind>& qualifiers) const final;

 private:
  QualifierKind qualifierKind_;
  const Id qualifiedTypeId_;
};

class Integer : public Type {
 public:
  enum class Encoding {
    BOOLEAN,
    SIGNED_INTEGER,
    UNSIGNED_INTEGER,
    SIGNED_CHARACTER,
    UNSIGNED_CHARACTER,
    UTF,
  };
  Integer(const std::string& name, Encoding encoding, uint32_t bitsize,
          uint32_t bytesize)
      : name_(name),
        bitsize_(bitsize),
        bytesize_(bytesize),
        encoding_(encoding) {}
  const std::string& GetName() const { return name_; }
  Encoding GetEncoding() const { return encoding_; }

  // GetBitSize() gives the semantics of the field. GetByteSize() gives the
  // storage size, and is equal or greater than GetBitSize()*8
  uint32_t GetBitSize() const { return bitsize_; }
  uint32_t GetByteSize() const { return bytesize_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;

 private:
  const std::string name_;
  const uint32_t bitsize_;
  const uint32_t bytesize_;
  const Encoding encoding_;
};

class Array : public Type {
 public:
  Array(Id elementTypeId,
        uint64_t numOfElements)
      : elementTypeId_(elementTypeId),
        numOfElements_(numOfElements) {}
  Id GetElementTypeId() const { return elementTypeId_; }
  uint64_t GetNumberOfElements() const { return numOfElements_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
  std::optional<Id> ResolveQualifier(
      std::set<QualifierKind>& qualifiers) const final;

 private:
  const Id elementTypeId_;
  const uint64_t numOfElements_;
};

class Member : public Type {
 public:
  Member(const std::string& name, Id typeId, uint64_t offset, uint64_t bitsize)
      : name_(name),
        typeId_(typeId),
        offset_(offset),
        bitsize_(bitsize) {}
  const std::string& GetName() const { return name_; }
  Id GetMemberType() const { return typeId_; }
  uint64_t GetOffset() const { return offset_; }
  uint64_t GetBitSize() const { return bitsize_; }
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
  std::string FirstName(const Graph& graph) const final;

 private:
  const std::string name_;
  const Id typeId_;
  const uint64_t offset_;
  const uint64_t bitsize_;
};

class StructUnion : public Type {
 public:
  struct Definition {
    const uint64_t bytesize;
    const std::vector<Id> members;
  };
  StructUnion(const std::string& name, StructUnionKind structUnionKind)
      : name_(name),
        structUnionKind_(structUnionKind) {}
  StructUnion(const std::string& name, StructUnionKind structUnionKind,
              uint64_t bytesize, const std::vector<Id>& members)
      : name_(name),
        structUnionKind_(structUnionKind),
        definition_({bytesize, members}) {}
  const std::string& GetName() const { return name_; }
  StructUnionKind GetStructUnionKind() const { return structUnionKind_; }
  const std::optional<Definition>& GetDefinition() const { return definition_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
  std::string FirstName(const Graph& graph) const final;

 private:
  std::vector<std::pair<std::string, size_t>> GetMemberNames(
      const Graph& graph) const;
  const std::string name_;
  const StructUnionKind structUnionKind_;
  const std::optional<Definition> definition_;
};

class Enumeration : public Type {
 public:
  using Enumerators = std::vector<std::pair<std::string, int64_t>>;
  struct Definition {
    const uint32_t bytesize;
    const Enumerators enumerators;
  };
  Enumeration(const std::string& name)
      : name_(name) {}
  Enumeration(const std::string& name, uint32_t bytesize,
              const Enumerators& enumerators)
      : name_(name),
        definition_({bytesize, enumerators}) {}
  const std::string& GetName() const { return name_; }
  const std::optional<Definition>& GetDefinition() const { return definition_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;

 private:
  std::vector<std::pair<std::string, size_t>> GetEnumNames() const;
  const std::string name_;
  const std::optional<Definition> definition_;
};

class Function : public Type {
 public:
  Function(Id returnTypeId,
           const std::vector<Parameter>& parameters)
      : returnTypeId_(returnTypeId),
        parameters_(parameters) {}
  Id GetReturnTypeId() const { return returnTypeId_; }
  const std::vector<Parameter>& GetParameters() const { return parameters_; }
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;
  std::optional<Id> ResolveQualifier(
      std::set<QualifierKind>& qualifiers) const final;

 private:
  const Id returnTypeId_;
  const std::vector<Parameter> parameters_;
};

class ElfSymbol : public Type {
 public:
  ElfSymbol(abigail::elf_symbol_sptr symbol, std::optional<Id> type_id)
      : symbol_(symbol),
        type_id_(type_id) {}
  abigail::elf_symbol_sptr GetElfSymbol() const { return symbol_; }
  std::optional<Id> GetTypeId() const { return type_id_; }
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;

 private:
  abigail::elf_symbol_sptr symbol_;
  std::optional<Id> type_id_;
};

class Symbols : public Type {
 public:
  Symbols(const std::map<std::string, Id>& symbols)
      : symbols_(symbols) {}
  std::string GetKindDescription() const final;
  Name MakeDescription(const Graph& graph, NameCache& names) const final;
  Result Equals(State& state, const Type& other) const final;

 private:
  std::map<std::string, Id> symbols_;
};

std::ostream& operator<<(std::ostream& os, Integer::Encoding encoding);

}  // namespace stg

#endif  // STG_STG_H_
