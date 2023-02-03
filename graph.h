// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2023 Google LLC
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

#ifndef STG_GRAPH_H_
#define STG_GRAPH_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "error.h"

namespace stg {

// A wrapped (for type safety) array index.
struct Id {
  // defined in graph.cc as maximum value for index type
  static const Id kInvalid;
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

using Pair = std::pair<Id, Id>;

}  // namespace stg

namespace std {

template <>
struct hash<stg::Id> {
  size_t operator()(const stg::Id& id) const {
    return hash<decltype(id.ix_)>()(id.ix_);
  }
};

template <>
struct hash<stg::Pair> {
  size_t operator()(const stg::Pair& comparison) const {
    const hash<stg::Id> h;
    auto h1 = h(comparison.first);
    auto h2 = h(comparison.second);
    // assumes 64-bit size_t, would be better if std::hash_combine existed
    return h1 ^ (h2 + 0x9e3779b97f4a7c15 + (h1 << 12) + (h1 >> 4));
  }
};

}  // namespace std

namespace stg {

struct Void {
};

struct Variadic {
};

struct PointerReference {
  enum class Kind {
    POINTER,
    LVALUE_REFERENCE,
    RVALUE_REFERENCE,
  };
  PointerReference(Kind kind, Id pointee_type_id)
      : kind(kind), pointee_type_id(pointee_type_id) {}

  Kind kind;
  Id pointee_type_id;
};

std::ostream& operator<<(std::ostream& os, PointerReference::Kind kind);

struct Typedef {
  Typedef(const std::string& name, Id referred_type_id)
      : name(name), referred_type_id(referred_type_id) {}

  std::string name;
  Id referred_type_id;
};

enum class Qualifier { CONST, VOLATILE, RESTRICT };

std::ostream& operator<<(std::ostream& os, Qualifier qualifier);

struct Qualified {
  Qualified(Qualifier qualifier, Id qualified_type_id)
      : qualifier(qualifier), qualified_type_id(qualified_type_id) {}

  Qualifier qualifier;
  Id qualified_type_id;
};

struct Primitive {
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
            uint32_t bytesize)
      : name(name), encoding(encoding), bytesize(bytesize) {}

  std::string name;
  std::optional<Encoding> encoding;
  uint32_t bytesize;
};

struct Array {
  Array(uint64_t number_of_elements, Id element_type_id)
      : number_of_elements(number_of_elements),
        element_type_id(element_type_id)  {}

  uint64_t number_of_elements;
  Id element_type_id;
};

struct BaseClass {
  enum class Inheritance { NON_VIRTUAL, VIRTUAL };
  BaseClass(Id type_id, uint64_t offset, Inheritance inheritance)
      : type_id(type_id), offset(offset), inheritance(inheritance) {}

  Id type_id;
  uint64_t offset;
  Inheritance inheritance;
};

std::ostream& operator<<(std::ostream& os, BaseClass::Inheritance inheritance);

struct Method {
  enum class Kind { NON_VIRTUAL, STATIC, VIRTUAL };
  Method(const std::string& mangled_name, const std::string& name, Kind kind,
         const std::optional<uint64_t> vtable_offset, Id type_id)
      : mangled_name(mangled_name), name(name), kind(kind),
        vtable_offset(vtable_offset), type_id(type_id) {}

  std::string mangled_name;
  std::string name;
  Kind kind;
  std::optional<uint64_t> vtable_offset;
  Id type_id;
};

std::ostream& operator<<(std::ostream& os, Method::Kind kind);

struct Member {
  Member(const std::string& name, Id type_id, uint64_t offset, uint64_t bitsize)
      : name(name), type_id(type_id), offset(offset), bitsize(bitsize) {}

  std::string name;
  Id type_id;
  uint64_t offset;
  uint64_t bitsize;
};

struct StructUnion {
  enum class Kind { STRUCT, UNION };
  struct Definition {
    uint64_t bytesize;
    std::vector<Id> base_classes;
    std::vector<Id> methods;
    std::vector<Id> members;
  };
  StructUnion(Kind kind, const std::string& name) : kind(kind), name(name) {}
  StructUnion(Kind kind, const std::string& name, uint64_t bytesize,
              const std::vector<Id>& base_classes,
              const std::vector<Id>& methods, const std::vector<Id>& members)
      : kind(kind), name(name),
        definition({bytesize, base_classes, methods, members}) {}

  Kind kind;
  std::string name;
  std::optional<Definition> definition;
};

std::ostream& operator<<(std::ostream& os, StructUnion::Kind kind);

struct Enumeration {
  using Enumerators = std::vector<std::pair<std::string, int64_t>>;
  struct Definition {
    Id underlying_type_id;
    Enumerators enumerators;
  };
  explicit Enumeration(const std::string& name) : name(name) {}
  Enumeration(const std::string& name, Id underlying_type_id,
              const Enumerators& enumerators)
      : name(name), definition({underlying_type_id, enumerators}) {}

  std::string name;
  std::optional<Definition> definition;
};

struct Function {
  Function(Id return_type_id, const std::vector<Id>& parameters)
      : return_type_id(return_type_id), parameters(parameters) {}

  Id return_type_id;
  std::vector<Id> parameters;
};

struct ElfSymbol {
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
    explicit CRC(uint32_t number) : number(number) {}
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

  std::string symbol_name;
  std::optional<VersionInfo> version_info;
  bool is_defined;
  SymbolType symbol_type;
  Binding binding;
  Visibility visibility;
  std::optional<CRC> crc;
  std::optional<std::string> ns;
  std::optional<Id> type_id;
  std::optional<std::string> full_name;
};

std::ostream& operator<<(std::ostream& os, ElfSymbol::SymbolType);
std::ostream& operator<<(std::ostream& os, ElfSymbol::Binding);
std::ostream& operator<<(std::ostream& os, ElfSymbol::Visibility);

std::string VersionInfoToString(const ElfSymbol::VersionInfo& version_info);
std::string VersionedSymbolName(const ElfSymbol&);

std::ostream& operator<<(std::ostream& os, ElfSymbol::CRC crc);

struct Symbols {
  explicit Symbols(const std::map<std::string, Id>& symbols)
      : symbols(symbols) {}

  std::map<std::string, Id> symbols;
};

std::ostream& operator<<(std::ostream& os, Primitive::Encoding encoding);

// Concrete graph type.
class Graph {
 public:
  bool Is(Id id) const {
    return indirection_[id.ix_].first != Which::ABSENT;
  }

  Id Allocate() {
    const auto ix = indirection_.size();
    indirection_.emplace_back(Which::ABSENT, 0);
    return Id(ix);
  }

  template <typename Node, typename... Args>
  void Set(Id id, Args&&... args) {
    auto& reference = indirection_[id.ix_];
    Check(reference.first == Which::ABSENT) << "node value already set";
    if constexpr (std::is_same_v<Node, Void>) {
      reference = {Which::VOID, void_.size()};
      void_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Variadic>) {
      reference = {Which::VARIADIC, variadic_.size()};
      variadic_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, PointerReference>) {
      reference = {Which::POINTER_REFERENCE, pointer_reference_.size()};
      pointer_reference_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Typedef>) {
      reference = {Which::TYPEDEF, typedef_.size()};
      typedef_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Qualified>) {
      reference = {Which::QUALIFIED, qualified_.size()};
      qualified_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Primitive>) {
      reference = {Which::PRIMITIVE, primitive_.size()};
      primitive_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Array>) {
      reference = {Which::ARRAY, array_.size()};
      array_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, BaseClass>) {
      reference = {Which::BASE_CLASS, base_class_.size()};
      base_class_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Method>) {
      reference = {Which::METHOD, method_.size()};
      method_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Member>) {
      reference = {Which::MEMBER, member_.size()};
      member_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, StructUnion>) {
      reference = {Which::STRUCT_UNION, struct_union_.size()};
      struct_union_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Enumeration>) {
      reference = {Which::ENUMERATION, enumeration_.size()};
      enumeration_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Function>) {
      reference = {Which::FUNCTION, function_.size()};
      function_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, ElfSymbol>) {
      reference = {Which::ELF_SYMBOL, elf_symbol_.size()};
      elf_symbol_.emplace_back(std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<Node, Symbols>) {
      reference = {Which::SYMBOLS, symbols_.size()};
      symbols_.emplace_back(std::forward<Args>(args)...);
    } else {
      // unfortunately we cannot static_assert(false, "missing case")
      static_assert(std::is_same<Node, Node*>::value, "missing case");
    }
  }

  template <typename Node, typename... Args>
  Id Add(Args&&... args) {
    auto id = Allocate();
    Set<Node>(id, std::forward<Args>(args)...);
    return id;
  }

  void Deallocate(Id) {
    // don't actually do anything, not supported
  }

  void Unset(Id id) {
    auto& reference = indirection_[id.ix_];
    Check(reference.first != Which::ABSENT) << "node value already unset";
    reference = {Which::ABSENT, 0};
  }

  void Remove(Id id) {
    Unset(id);
    Deallocate(id);
  }

  template <typename Result, typename FunctionObject, typename... Args>
  Result Apply(FunctionObject& function, Id id, Args&&... args) const;

  template <typename Result, typename FunctionObject, typename... Args>
  Result Apply2(FunctionObject& function, Id id1, Id id2, Args&&... args) const;

  template <typename Result, typename FunctionObject, typename... Args>
  Result Apply(FunctionObject& function, Id id, Args&&... args);

 private:
  enum class Which {
    ABSENT,
    VOID,
    VARIADIC,
    POINTER_REFERENCE,
    TYPEDEF,
    QUALIFIED,
    PRIMITIVE,
    ARRAY,
    BASE_CLASS,
    METHOD,
    MEMBER,
    STRUCT_UNION,
    ENUMERATION,
    FUNCTION,
    ELF_SYMBOL,
    SYMBOLS,
  };

  std::vector<std::pair<Which, size_t>> indirection_;

  std::vector<Void> void_;
  std::vector<Variadic> variadic_;
  std::vector<PointerReference> pointer_reference_;
  std::vector<Typedef> typedef_;
  std::vector<Qualified> qualified_;
  std::vector<Primitive> primitive_;
  std::vector<Array> array_;
  std::vector<BaseClass> base_class_;
  std::vector<Method> method_;
  std::vector<Member> member_;
  std::vector<StructUnion> struct_union_;
  std::vector<Enumeration> enumeration_;
  std::vector<Function> function_;
  std::vector<ElfSymbol> elf_symbol_;
  std::vector<Symbols> symbols_;
};

template <typename Result, typename FunctionObject, typename... Args>
Result Graph::Apply(FunctionObject& function, Id id, Args&&... args) const {
  const auto& [which, ix] = indirection_[id.ix_];
  switch (which) {
    case Which::ABSENT:
      Die() << "undefined node";
    case Which::VOID:
      return function(void_[ix], std::forward<Args>(args)...);
    case Which::VARIADIC:
      return function(variadic_[ix], std::forward<Args>(args)...);
    case Which::POINTER_REFERENCE:
      return function(pointer_reference_[ix], std::forward<Args>(args)...);
    case Which::TYPEDEF:
      return function(typedef_[ix], std::forward<Args>(args)...);
    case Which::QUALIFIED:
      return function(qualified_[ix], std::forward<Args>(args)...);
    case Which::PRIMITIVE:
      return function(primitive_[ix], std::forward<Args>(args)...);
    case Which::ARRAY:
      return function(array_[ix], std::forward<Args>(args)...);
    case Which::BASE_CLASS:
      return function(base_class_[ix], std::forward<Args>(args)...);
    case Which::METHOD:
      return function(method_[ix], std::forward<Args>(args)...);
    case Which::MEMBER:
      return function(member_[ix], std::forward<Args>(args)...);
    case Which::STRUCT_UNION:
      return function(struct_union_[ix], std::forward<Args>(args)...);
    case Which::ENUMERATION:
      return function(enumeration_[ix], std::forward<Args>(args)...);
    case Which::FUNCTION:
      return function(function_[ix], std::forward<Args>(args)...);
    case Which::ELF_SYMBOL:
      return function(elf_symbol_[ix], std::forward<Args>(args)...);
    case Which::SYMBOLS:
      return function(symbols_[ix], std::forward<Args>(args)...);
  }
}

template <typename Result, typename FunctionObject, typename... Args>
Result Graph::Apply2(
    FunctionObject& function, Id id1, Id id2, Args&&... args) const {
  const auto& [which1, ix1] = indirection_[id1.ix_];
  const auto& [which2, ix2] = indirection_[id2.ix_];
  if (which1 != which2) {
    return function.Mismatch(std::forward<Args>(args)...);
  }
  switch (which1) {
    case Which::ABSENT:
      Die() << "undefined node";
    case Which::VOID:
      return function(void_[ix1], void_[ix2],
                      std::forward<Args>(args)...);
    case Which::VARIADIC:
      return function(variadic_[ix1], variadic_[ix2],
                      std::forward<Args>(args)...);
    case Which::POINTER_REFERENCE:
      return function(pointer_reference_[ix1], pointer_reference_[ix2],
                      std::forward<Args>(args)...);
    case Which::TYPEDEF:
      return function(typedef_[ix1], typedef_[ix2],
                      std::forward<Args>(args)...);
    case Which::QUALIFIED:
      return function(qualified_[ix1], qualified_[ix2],
                      std::forward<Args>(args)...);
    case Which::PRIMITIVE:
      return function(primitive_[ix1], primitive_[ix2],
                      std::forward<Args>(args)...);
    case Which::ARRAY:
      return function(array_[ix1], array_[ix2],
                      std::forward<Args>(args)...);
    case Which::BASE_CLASS:
      return function(base_class_[ix1], base_class_[ix2],
                      std::forward<Args>(args)...);
    case Which::METHOD:
      return function(method_[ix1], method_[ix2],
                      std::forward<Args>(args)...);
    case Which::MEMBER:
      return function(member_[ix1], member_[ix2],
                      std::forward<Args>(args)...);
    case Which::STRUCT_UNION:
      return function(struct_union_[ix1], struct_union_[ix2],
                      std::forward<Args>(args)...);
    case Which::ENUMERATION:
      return function(enumeration_[ix1], enumeration_[ix2],
                      std::forward<Args>(args)...);
    case Which::FUNCTION:
      return function(function_[ix1], function_[ix2],
                      std::forward<Args>(args)...);
    case Which::ELF_SYMBOL:
      return function(elf_symbol_[ix1], elf_symbol_[ix2],
                      std::forward<Args>(args)...);
    case Which::SYMBOLS:
      return function(symbols_[ix1], symbols_[ix2],
                      std::forward<Args>(args)...);
  }
}

template <typename Result, typename FunctionObject, typename... Args>
struct ConstAdapter {
  explicit ConstAdapter(FunctionObject& function) : function(function) {}
  template <typename Node>
  Result operator()(const Node& node, Args&&... args) {
    return function(const_cast<Node&>(node), std::forward<Args>(args)...);
  }
  FunctionObject& function;
};

template <typename Result, typename FunctionObject, typename... Args>
Result Graph::Apply(FunctionObject& function, Id id, Args&&... args) {
  ConstAdapter<Result, FunctionObject, Args&&...> adapter(function);
  return static_cast<const Graph&>(*this).Apply<Result>(
      adapter, id, std::forward<Args>(args)...);
}

}  // namespace stg

#endif  // STG_GRAPH_H_
