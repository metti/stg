// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2022 Google LLC
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
// Author: Siddharth Nayyar

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "graph.h"
#include "stg.proto.h"

namespace stg {
namespace proto {

namespace {

struct Transform {
  Transform(const Graph& graph, proto::STG& stg) : graph(graph), stg(stg) {}

  uint32_t operator()(Id);

  void operator()(const stg::Void&, uint32_t);
  void operator()(const stg::Variadic&, uint32_t);
  void operator()(const stg::PointerReference&, uint32_t);
  void operator()(const stg::Typedef&, uint32_t);
  void operator()(const stg::Qualified&, uint32_t);
  void operator()(const stg::Primitive&, uint32_t);
  void operator()(const stg::Array&, uint32_t);
  void operator()(const stg::BaseClass&, uint32_t);
  void operator()(const stg::Member&, uint32_t);
  void operator()(const stg::Method&, uint32_t);
  void operator()(const stg::StructUnion&, uint32_t);
  void operator()(const stg::Enumeration&, uint32_t);
  void operator()(const stg::Function&, uint32_t);
  void operator()(const stg::ElfSymbol&, uint32_t);
  void operator()(const stg::Symbols&, uint32_t);

  PointerReference::Kind operator()(stg::PointerReference::Kind);
  Qualified::Qualifier operator()(stg::Qualifier);
  Primitive::Encoding operator()(stg::Primitive::Encoding);
  BaseClass::Inheritance operator()(stg::BaseClass::Inheritance);
  Method::Kind operator()(stg::Method::Kind);
  StructUnion::Kind operator()(stg::StructUnion::Kind);
  ElfSymbol::SymbolType operator()(stg::ElfSymbol::SymbolType);
  ElfSymbol::Binding operator()(stg::ElfSymbol::Binding);
  ElfSymbol::Visibility operator()(stg::ElfSymbol::Visibility);

  const Graph& graph;
  proto::STG& stg;
  std::unordered_map<Id, uint32_t> external_id;
};

uint32_t Transform::operator()(Id id) {
  auto [it, inserted] = external_id.insert({id, id.ix_});
  if (inserted) {
    graph.Apply<void>(*this, id, it->second);
  }
  return it->second;
}

void Transform::operator()(const stg::Void&, uint32_t id) {
  stg.add_void_()->set_id(id);
}

void Transform::operator()(const stg::Variadic&, uint32_t id) {
  stg.add_variadic()->set_id(id);
}

void Transform::operator()(const stg::PointerReference& x, uint32_t id) {
  auto& pointer_reference = *stg.add_pointer_reference();
  pointer_reference.set_id(id);
  pointer_reference.set_kind((*this)(x.kind));
  pointer_reference.set_pointee_type_id((*this)(x.pointee_type_id));
}

void Transform::operator()(const stg::Typedef& x, uint32_t id) {
  auto& typedef_ = *stg.add_typedef_();
  typedef_.set_id(id);
  typedef_.set_name(x.name);
  typedef_.set_referred_type_id((*this)(x.referred_type_id));
}

void Transform::operator()(const stg::Qualified& x, uint32_t id) {
  auto& qualified = *stg.add_qualified();
  qualified.set_id(id);
  qualified.set_qualifier((*this)(x.qualifier));
  qualified.set_qualified_type_id((*this)(x.qualified_type_id));
}

void Transform::operator()(const stg::Primitive& x, uint32_t id) {
  auto& primitive = *stg.add_primitive();
  primitive.set_id(id);
  primitive.set_name(x.name);
  if (x.encoding) {
    primitive.set_encoding((*this)(*x.encoding));
  }
  primitive.set_bitsize(x.bitsize);
  primitive.set_bytesize(x.bytesize);
}

void Transform::operator()(const stg::Array& x, uint32_t id) {
  auto& array = *stg.add_array();
  array.set_id(id);
  array.set_number_of_elements(x.number_of_elements);
  array.set_element_type_id((*this)(x.element_type_id));
}

void Transform::operator()(const stg::BaseClass& x, uint32_t id) {
  auto& base_class = *stg.add_base_class();
  base_class.set_id(id);
  base_class.set_type_id((*this)(x.type_id));
  base_class.set_offset(x.offset);
  base_class.set_inheritance((*this)(x.inheritance));
}

void Transform::operator()(const stg::Member& x, uint32_t id) {
  auto& member = *stg.add_member();
  member.set_id(id);
  member.set_name(x.name);
  member.set_type_id((*this)(x.type_id));
  member.set_offset(x.offset);
  member.set_bitsize(x.bitsize);
}

void Transform::operator()(const stg::Method& x, uint32_t id) {
  auto& method = *stg.add_method();
  method.set_id(id);
  method.set_mangled_name(x.mangled_name);
  method.set_name(x.name);
  method.set_kind((*this)(x.kind));
  if (x.vtable_offset) {
    method.set_vtable_offset(*x.vtable_offset);
  }
  method.set_type_id((*this)(x.type_id));
}

void Transform::operator()(const stg::StructUnion& x, uint32_t id) {
  auto& struct_union = *stg.add_struct_union();
  struct_union.set_id(id);
  struct_union.set_kind((*this)(x.kind));
  struct_union.set_name(x.name);
  if (x.definition) {
    auto& definition = *struct_union.mutable_definition();
    definition.set_bytesize(x.definition->bytesize);
    for (const auto id : x.definition->base_classes) {
      definition.add_base_class_id((*this)(id));
    }
    for (const auto id : x.definition->members) {
      definition.add_member_id((*this)(id));
    }
    for (const auto id : x.definition->methods) {
      definition.add_method_id((*this)(id));
    }
  }
}

void Transform::operator()(const stg::Enumeration& x, uint32_t id) {
  auto& enumeration = *stg.add_enumeration();
  enumeration.set_id(id);
  enumeration.set_name(x.name);
  if (x.definition) {
    auto& definition = *enumeration.mutable_definition();
    definition.set_bytesize(x.definition->bytesize);
    for (const auto& [name, value] : x.definition->enumerators) {
      auto& enumerator = *definition.add_enumerator();
      enumerator.set_name(name);
      enumerator.set_value(value);
    }
  }
}

void Transform::operator()(const stg::Function& x, uint32_t id) {
  auto& function = *stg.add_function();
  function.set_id(id);
  function.set_return_type_id((*this)(x.return_type_id));
  for (const auto id : x.parameters) {
    function.add_parameter_id((*this)(id));
  }
}

void Transform::operator()(const stg::ElfSymbol& x, uint32_t id) {
  auto& elf_symbol = *stg.add_elf_symbol();
  elf_symbol.set_id(id);
  elf_symbol.set_name(x.symbol_name);
  if (x.version_info) {
    auto& version_info = *elf_symbol.mutable_version_info();
    version_info.set_is_default(x.version_info->is_default);
    version_info.set_name(x.version_info->name);
  }
  elf_symbol.set_is_defined(x.is_defined);
  elf_symbol.set_symbol_type((*this)(x.symbol_type));
  elf_symbol.set_binding((*this)(x.binding));
  elf_symbol.set_visibility((*this)(x.visibility));
  if (x.crc) {
    elf_symbol.set_crc(x.crc->number);
  }
  if (x.ns) {
    elf_symbol.set_namespace_(*x.ns);
  }
  if (x.type_id) {
    elf_symbol.set_type_id((*this)(*x.type_id));
  }
  if (x.full_name) {
    elf_symbol.set_full_name(*x.full_name);
  }
}

void Transform::operator()(const stg::Symbols& x, uint32_t id) {
  auto& symbols = *stg.mutable_symbols();
  symbols.set_id(id);
  for (const auto& [symbol, id] : x.symbols) {
    (*symbols.mutable_symbols())[symbol] = (*this)(id);
  }
}

PointerReference::Kind Transform::operator()(stg::PointerReference::Kind x) {
  switch (x) {
    case stg::PointerReference::Kind::POINTER:
      return PointerReference::POINTER;
    case stg::PointerReference::Kind::LVALUE_REFERENCE:
      return PointerReference::LVALUE_REFERENCE;
    case stg::PointerReference::Kind::RVALUE_REFERENCE:
      return PointerReference::RVALUE_REFERENCE;
  }
}

Qualified::Qualifier Transform::operator()(stg::Qualifier x) {
  switch (x) {
    case stg::Qualifier::CONST:
      return Qualified::CONST;
    case stg::Qualifier::VOLATILE:
      return Qualified::VOLATILE;
    case stg::Qualifier::RESTRICT:
      return Qualified::RESTRICT;
  }
}

Primitive::Encoding Transform::operator()(stg::Primitive::Encoding x) {
  switch (x) {
    case stg::Primitive::Encoding::BOOLEAN:
      return Primitive::BOOLEAN;
    case stg::Primitive::Encoding::SIGNED_INTEGER:
      return Primitive::SIGNED_INTEGER;
    case stg::Primitive::Encoding::UNSIGNED_INTEGER:
      return Primitive::UNSIGNED_INTEGER;
    case stg::Primitive::Encoding::SIGNED_CHARACTER:
      return Primitive::SIGNED_CHARACTER;
    case stg::Primitive::Encoding::UNSIGNED_CHARACTER:
      return Primitive::UNSIGNED_CHARACTER;
    case stg::Primitive::Encoding::REAL_NUMBER:
      return Primitive::REAL_NUMBER;
    case stg::Primitive::Encoding::COMPLEX_NUMBER:
      return Primitive::COMPLEX_NUMBER;
    case stg::Primitive::Encoding::UTF:
      return Primitive::UTF;
  }
}

BaseClass::Inheritance Transform::operator()(stg::BaseClass::Inheritance x) {
  switch (x) {
    case stg::BaseClass::Inheritance::NON_VIRTUAL:
      return BaseClass::NON_VIRTUAL;
    case stg::BaseClass::Inheritance::VIRTUAL:
      return BaseClass::VIRTUAL;
  }
}

Method::Kind Transform::operator()(stg::Method::Kind x) {
  switch (x) {
    case stg::Method::Kind::NON_VIRTUAL:
      return Method::NON_VIRTUAL;
    case stg::Method::Kind::STATIC:
      return Method::STATIC;
    case stg::Method::Kind::VIRTUAL:
      return Method::VIRTUAL;
  }
}

StructUnion::Kind Transform::operator()(stg::StructUnion::Kind x) {
  switch (x) {
    case stg::StructUnion::Kind::CLASS:
      return StructUnion::CLASS;
    case stg::StructUnion::Kind::STRUCT:
      return StructUnion::STRUCT;
    case stg::StructUnion::Kind::UNION:
      return StructUnion::UNION;
  }
}

ElfSymbol::SymbolType Transform::operator()(stg::ElfSymbol::SymbolType x) {
  switch (x) {
    case stg::ElfSymbol::SymbolType::OBJECT:
      return ElfSymbol::OBJECT;
    case stg::ElfSymbol::SymbolType::FUNCTION:
      return ElfSymbol::FUNCTION;
    case stg::ElfSymbol::SymbolType::COMMON:
      return ElfSymbol::COMMON;
    case stg::ElfSymbol::SymbolType::TLS:
      return ElfSymbol::TLS;
  }
}

ElfSymbol::Binding Transform::operator()(stg::ElfSymbol::Binding x) {
  switch (x) {
    case stg::ElfSymbol::Binding::GLOBAL:
      return ElfSymbol::GLOBAL;
    case stg::ElfSymbol::Binding::LOCAL:
      return ElfSymbol::LOCAL;
    case stg::ElfSymbol::Binding::WEAK:
      return ElfSymbol::WEAK;
    case stg::ElfSymbol::Binding::GNU_UNIQUE:
      return ElfSymbol::GNU_UNIQUE;
  }
}

ElfSymbol::Visibility Transform::operator()(stg::ElfSymbol::Visibility x) {
  switch (x) {
    case stg::ElfSymbol::Visibility::DEFAULT:
      return ElfSymbol::DEFAULT;
    case stg::ElfSymbol::Visibility::PROTECTED:
      return ElfSymbol::PROTECTED;
    case stg::ElfSymbol::Visibility::HIDDEN:
      return ElfSymbol::HIDDEN;
    case stg::ElfSymbol::Visibility::INTERNAL:
      return ElfSymbol::INTERNAL;
  }
}

}  // namespace

}  // namespace proto
}  // namespace stg
