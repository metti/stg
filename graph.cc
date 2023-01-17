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

#include "graph.h"

#include <ios>
#include <ostream>
#include <string>

namespace stg {

std::ostream& operator<<(std::ostream& os, Id id) {
  return os << '<' << id.ix_ << '>';
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

std::ostream& operator<<(std::ostream& os, ElfSymbol::CRC crc) {
  return os << "0x" << std::hex << crc.number;
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
