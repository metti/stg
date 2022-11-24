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

#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "error.h"

namespace stg {

// A wrapped (for type safety) array index.
struct Id {
  explicit Id(size_t ix) : ix_(ix) {}
  // TODO: auto operator<=>(const Id&) const = default;
  bool operator==(const Id& other) const {
    return ix_ == other.ix_;
  }
  bool operator!=(const Id& other) const {
    return ix_ != other.ix_;
  }
  size_t ix_;
};

std::ostream& operator<<(std::ostream& os, Id id);

}  // namespace stg

namespace std {

template<>
struct hash<stg::Id> {
  size_t operator()(const stg::Id& id) const {
    return id.ix_;
  }
};

}  // namespace std

namespace stg {

struct Node {
  Node() = default;
  Node(const Node&) = delete;
  Node(Node&&) = default;
  virtual ~Node() = default;
};

struct Void : Node {
};

struct Variadic : Node {
};

struct PointerReference : Node {
  enum class Kind {
    POINTER,
    LVALUE_REFERENCE,
    RVALUE_REFERENCE,
  };
  PointerReference(Kind kind, Id pointee_type_id)
      : kind(kind), pointee_type_id(pointee_type_id) {}

  const Kind kind;
  const Id pointee_type_id;
};

std::ostream& operator<<(std::ostream& os, PointerReference::Kind kind);

struct Typedef : Node {
  Typedef(const std::string& name, Id referred_type_id)
      : name(name), referred_type_id(referred_type_id) {}

  const std::string name;
  const Id referred_type_id;
};

enum class Qualifier { CONST, VOLATILE, RESTRICT };

std::ostream& operator<<(std::ostream& os, Qualifier qualifier);

struct Qualified : Node {
  Qualified(Qualifier qualifier, Id qualified_type_id)
      : qualifier(qualifier), qualified_type_id(qualified_type_id) {}

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

  const uint64_t number_of_elements;
  const Id element_type_id;
};

struct BaseClass : Node {
  enum class Inheritance { NON_VIRTUAL, VIRTUAL };
  BaseClass(Id type_id, uint64_t offset, Inheritance inheritance)
      : type_id(type_id), offset(offset), inheritance(inheritance) {}

  const Id type_id;
  const uint64_t offset;
  const Inheritance inheritance;
};

std::ostream& operator<<(std::ostream& os, BaseClass::Inheritance inheritance);

struct Member : Node {
  Member(const std::string& name, Id type_id, uint64_t offset, uint64_t bitsize)
      : name(name), type_id(type_id), offset(offset), bitsize(bitsize) {}

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

  const std::string name;
  const std::optional<Definition> definition;
};

struct Function : Node {
  Function(Id return_type_id, const std::vector<Id>& parameters)
      : return_type_id(return_type_id), parameters(parameters) {}

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
  struct CRC {
    // TODO: auto operator<=>(const bool&) const = default;
    bool operator==(const CRC& other) const {
      return number == other.number;
    }
    bool operator!=(const CRC& other) const {
      return number != other.number;
    }
    uint32_t number;
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

std::ostream& operator<<(std::ostream& os, ElfSymbol::CRC crc);

struct Symbols : Node {
  Symbols(const std::map<std::string, Id>& symbols) : symbols(symbols) {}

  const std::map<std::string, Id> symbols;
};

std::ostream& operator<<(std::ostream& os, Primitive::Encoding encoding);

// Concrete graph type.
class Graph {
 public:
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

  template <typename Result, typename FunctionObject, typename... Args>
  Result Apply(FunctionObject& function, Id id, Args&&... args) const;

  template <typename Result, typename FunctionObject, typename... Args>
  Result Apply2(FunctionObject& function, Id id1, Id id2, Args&&... args) const;

 private:
  const Node& Get(Id id) const {
    return *nodes_[id.ix_].get();
  }

  std::vector<std::unique_ptr<Node>> nodes_;
};

template <typename Result, typename FunctionObject, typename... Args>
Result Graph::Apply(FunctionObject& function, Id id, Args&&... args) const {
  const Node& node = Get(id);
  const auto& type_id = typeid(node);
  if (type_id == typeid(Void)) {
    return function(static_cast<const Void&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Variadic)) {
    return function(static_cast<const Variadic&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(PointerReference)) {
    return function(static_cast<const PointerReference&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Typedef)) {
    return function(static_cast<const Typedef&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Qualified)) {
    return function(static_cast<const Qualified&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Primitive)) {
    return function(static_cast<const Primitive&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Array)) {
    return function(static_cast<const Array&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(BaseClass)) {
    return function(static_cast<const BaseClass&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Member)) {
    return function(static_cast<const Member&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Method)) {
    return function(static_cast<const Method&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(StructUnion)) {
    return function(static_cast<const StructUnion&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Enumeration)) {
    return function(static_cast<const Enumeration&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Function)) {
    return function(static_cast<const Function&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(ElfSymbol)) {
    return function(static_cast<const ElfSymbol&>(node),
                    std::forward<Args>(args)...);
  }
  if (type_id == typeid(Symbols)) {
    return function(static_cast<const Symbols&>(node),
                    std::forward<Args>(args)...);
  }
  Die() << "unknown node type " << type_id.name();
}

template <typename Result, typename FunctionObject, typename... Args>
Result Graph::Apply2(
    FunctionObject& function, Id id1, Id id2, Args&&... args) const {
  const Node& node1 = Get(id1);
  const Node& node2 = Get(id2);
  const auto& type_id1 = typeid(node1);
  const auto& type_id2 = typeid(node2);
  if (type_id1 != type_id2) {
    return function.Mismatch(std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Void)) {
    return function(static_cast<const Void&>(node1),
                    static_cast<const Void&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Variadic)) {
    return function(static_cast<const Variadic&>(node1),
                    static_cast<const Variadic&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(PointerReference)) {
    return function(static_cast<const PointerReference&>(node1),
                    static_cast<const PointerReference&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Typedef)) {
    return function(static_cast<const Typedef&>(node1),
                    static_cast<const Typedef&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Qualified)) {
    return function(static_cast<const Qualified&>(node1),
                    static_cast<const Qualified&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Primitive)) {
    return function(static_cast<const Primitive&>(node1),
                    static_cast<const Primitive&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Array)) {
    return function(static_cast<const Array&>(node1),
                    static_cast<const Array&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(BaseClass)) {
    return function(static_cast<const BaseClass&>(node1),
                    static_cast<const BaseClass&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Member)) {
    return function(static_cast<const Member&>(node1),
                    static_cast<const Member&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Method)) {
    return function(static_cast<const Method&>(node1),
                    static_cast<const Method&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(StructUnion)) {
    return function(static_cast<const StructUnion&>(node1),
                    static_cast<const StructUnion&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Enumeration)) {
    return function(static_cast<const Enumeration&>(node1),
                    static_cast<const Enumeration&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Function)) {
    return function(static_cast<const Function&>(node1),
                    static_cast<const Function&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(ElfSymbol)) {
    return function(static_cast<const ElfSymbol&>(node1),
                    static_cast<const ElfSymbol&>(node2),
                    std::forward<Args>(args)...);
  }
  if (type_id1 == typeid(Symbols)) {
    return function(static_cast<const Symbols&>(node1),
                    static_cast<const Symbols&>(node2),
                    std::forward<Args>(args)...);
  }
  Die() << "unknown node type " << type_id1.name();
}

}  // namespace stg

#endif  // STG_STG_H_
