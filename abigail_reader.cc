// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2021 Google LLC
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
// Author: Giuliano Procida

#include "abigail_reader.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <utility>

#include <libxml/parser.h>
#include "crc.h"
#include "error.h"

namespace stg {
namespace abixml {

namespace {

const char* FromLibxml(const xmlChar* str) {
  return reinterpret_cast<const char*>(str);
}

const xmlChar* ToLibxml(const char* str) {
  return reinterpret_cast<const xmlChar*>(str);
}

std::string GetElementName(xmlNodePtr element) {
  return std::string(FromLibxml(element->name));
}

void CheckElementName(const char* name, xmlNodePtr element) {
  const auto element_name = FromLibxml(element->name);
  if (strcmp(element_name, name) != 0)
    Die() << "expected element '" << name
          << "' but got '" << element_name << "'";
}

xmlNodePtr GetOnlyChild(const std::string& name, xmlNodePtr element) {
  xmlNodePtr child = xmlFirstElementChild(element);
  Check(child && !xmlNextElementSibling(child))
      << name << " with not exactly one child element";
  return child;
}

std::optional<std::string> GetAttribute(xmlNodePtr node, const char* name) {
  std::optional<std::string> result;
  xmlChar* attribute = xmlGetProp(node, ToLibxml(name));
  if (attribute) {
    result = {FromLibxml(attribute)};
    xmlFree(attribute);
  }
  return result;
}

std::string GetAttributeOrDie(xmlNodePtr node, const char* name) {
  xmlChar* attribute = xmlGetProp(node, ToLibxml(name));
  if (!attribute)
    Die() << "element '" << FromLibxml(node->name)
          << "' missing attribute '" << name << "'";
  std::string result = FromLibxml(attribute);
  xmlFree(attribute);
  return result;
}

template <typename T>
std::optional<T> Parse(const std::string& value) {
  T result;
  std::istringstream is(value);
  is >> std::noskipws >> result;
  if (!is || !is.eof())
    return {};
  else
    return {result};
}

template <>
std::optional<bool> Parse<bool>(const std::string& value) {
  if (value == "yes")
    return {true};
  if (value == "no")
    return {false};
  return {};
}

template <>
std::optional<ElfSymbol::SymbolType> Parse<ElfSymbol::SymbolType>(
    const std::string& value) {
  if (value == "object-type")
    return {ElfSymbol::SymbolType::OBJECT};
  if (value == "func-type")
    return {ElfSymbol::SymbolType::FUNCTION};
  if (value == "common-type")
    return {ElfSymbol::SymbolType::COMMON};
  if (value == "tls-type")
    return {ElfSymbol::SymbolType::TLS};
  return {};
}

template <>
std::optional<ElfSymbol::Binding> Parse<ElfSymbol::Binding>(
    const std::string& value) {
  if (value == "global-binding")
    return {ElfSymbol::Binding::GLOBAL};
  if (value == "local-binding")
    return {ElfSymbol::Binding::LOCAL};
  if (value == "weak-binding")
    return {ElfSymbol::Binding::WEAK};
  if (value == "gnu-unique-binding")
    return {ElfSymbol::Binding::GNU_UNIQUE};
  return {};
}

template <>
std::optional<ElfSymbol::Visibility> Parse<ElfSymbol::Visibility>(
    const std::string& value) {
  if (value == "default-visibility")
    return {ElfSymbol::Visibility::DEFAULT};
  if (value == "protected-visibility")
    return {ElfSymbol::Visibility::PROTECTED};
  if (value == "hidden-visibility")
    return {ElfSymbol::Visibility::HIDDEN};
  if (value == "internal-visibility")
    return {ElfSymbol::Visibility::INTERNAL};
  return {};
}

template <>
std::optional<CRC> Parse<CRC>(const std::string& value) {
  CRC result;
  std::istringstream is(value);
  is >> std::noskipws >> std::hex >> result.number;
  if (!is || !is.eof())
    return {};
  else
    return {result};
}

template <typename T>
T GetParsedValueOrDie(xmlNodePtr element, const char* name,
                      const std::string& value, const std::optional<T>& parse) {
  if (!parse)
    Die() << "element '" << FromLibxml(element->name)
          << "' has attribute '" << name
          << "' with bad value '" << value << "'";
  return *parse;
}

template <typename T>
T ReadAttributeOrDie(xmlNodePtr element, const char* name) {
  const auto value = GetAttributeOrDie(element, name);
  return GetParsedValueOrDie(element, name, value, Parse<T>(value));
}

template <typename T>
std::optional<T> ReadAttribute(xmlNodePtr element, const char* name) {
  const auto value = GetAttribute(element, name);
  if (!value)
    return {};
  return {GetParsedValueOrDie(element, name, *value, Parse<T>(*value))};
}

template <typename T>
T ReadAttribute(xmlNodePtr element, const char* name, T default_value) {
  const auto value = GetAttribute(element, name);
  if (!value)
    return default_value;
  return GetParsedValueOrDie(element, name, *value, Parse<T>(*value));
}

template <typename T>
T ReadAttribute(xmlNodePtr element, const char* name,
                std::function<std::optional<T>(const std::string&)> parse) {
  const auto value = GetAttributeOrDie(element, name);
  return GetParsedValueOrDie(element, name, value, parse(value));
}

std::optional<uint64_t> ParseLength(const std::string& value) {
  if (value == "infinite")
    return {0};
  return Parse<uint64_t>(value);
}

std::optional<PointerReference::Kind> ParseReferenceKind(
    const std::string& value) {
  if (value == "lvalue")
    return {PointerReference::Kind::LVALUE_REFERENCE};
  if (value == "rvalue")
    return {PointerReference::Kind::RVALUE_REFERENCE};
  return {};
}

class PushScopeName {
 public:
  PushScopeName(std::string& scope_name, const std::string& name)
      : scope_name_(scope_name), old_size_(scope_name.size()) {
    scope_name_ += name;
    scope_name_ += "::";
  }
  PushScopeName(const PushScopeName& other) = delete;
  PushScopeName& operator=(const PushScopeName& other) = delete;
  ~PushScopeName() { scope_name_.resize(old_size_); }

 private:
  std::string& scope_name_;
  const size_t old_size_;
};

}  // namespace

Abigail::Abigail(Graph& graph, bool verbose)
    : graph_(graph), verbose_(verbose) { }

Id Abigail::GetNode(const std::string& type_id) {
  const auto [it, inserted] = type_ids_.insert({type_id, Id(0)});
  if (inserted)
    it->second = graph_.Allocate();
  return it->second;
}

Id Abigail::GetEdge(xmlNodePtr element) {
  return GetNode(GetAttributeOrDie(element, "type-id"));
}

Id Abigail::GetVariadic() {
  if (!variadic_) {
    variadic_ = {graph_.Add(Make<Variadic>())};
    if (verbose_)
      std::cerr << *variadic_ << " variadic parameter\n";
  }
  return *variadic_;
}

std::unique_ptr<Node> Abigail::MakeFunctionType(xmlNodePtr function) {
  std::vector<Parameter> parameters;
  std::optional<Id> return_type;
  for (auto child = xmlFirstElementChild(function); child;
       child = xmlNextElementSibling(child)) {
    const auto child_name = GetElementName(child);
    if (return_type)
      Die() << "unexpected element after return-type";
    if (child_name == "parameter") {
      const auto is_variadic = ReadAttribute<bool>(child, "is-variadic", false);
      if (is_variadic) {
        const auto type = GetVariadic();
        Parameter parameter{.name = std::string(), .type_id = type};
        parameters.push_back(std::move(parameter));
      } else {
        const auto name = GetAttribute(child, "name");
        const auto type = GetEdge(child);
        Parameter parameter{.name = name ? *name : std::string(),
                            .type_id = type};
        parameters.push_back(std::move(parameter));
      }
    } else if (child_name == "return") {
      return_type = {GetEdge(child)};
    } else {
      Die() << "unrecognised " << FromLibxml(function->name)
            << " child element '" << child_name << "'";
    }
  }
  if (!return_type)
    Die() << "missing return-type";
  if (verbose_) {
    std::cerr << "  made function type (";
    bool comma = false;
    for (const auto& p : parameters) {
      if (comma)
        std::cerr << ", ";
      std::cerr << p.type_id;
      comma = true;
    }
    std::cerr << ") -> " << *return_type << "\n";
  }
  return Make<Function>(*return_type, parameters);
}

Id Abigail::ProcessRoot(xmlNodePtr root) {
  const auto name = GetElementName(root);
  if (name == "abi-corpus-group") {
    ProcessCorpusGroup(root);
  } else if (name == "abi-corpus") {
    ProcessCorpus(root);
  } else {
    Die() << "unrecognised root element '" << name << "'";
  }
  return BuildSymbols();
}

void Abigail::ProcessCorpusGroup(xmlNodePtr group) {
  for (auto corpus = xmlFirstElementChild(group); corpus;
       corpus = xmlNextElementSibling(corpus)) {
    CheckElementName("abi-corpus", corpus);
    ProcessCorpus(corpus);
  }
}

void Abigail::ProcessCorpus(xmlNodePtr corpus) {
  for (auto element = xmlFirstElementChild(corpus); element;
       element = xmlNextElementSibling(element)) {
    const auto name = GetElementName(element);
    if (name == "elf-function-symbols" || name == "elf-variable-symbols") {
      ProcessSymbols(element);
    } else if (name == "elf-needed") {
      // ignore this
    } else if (name == "abi-instr") {
      ProcessInstr(element);
    } else {
      Die() << "unrecognised abi-corpus child element '" << name << "'";
    }
  }
}

void Abigail::ProcessSymbols(xmlNodePtr symbols) {
  for (auto element = xmlFirstElementChild(symbols); element;
       element = xmlNextElementSibling(element)) {
    CheckElementName("elf-symbol", element);
    ProcessSymbol(element);
  }
}

void Abigail::ProcessSymbol(xmlNodePtr symbol) {
  // Symbol processing is done in two parts. In this first part, we parse just
  // enough XML attributes to generate a symbol id and determine any aliases.
  // Symbol ids in this format can be found in elf-symbol alias attributes and
  // in {var,function}-decl elf-symbol-id attributes.
  const auto name = GetAttributeOrDie(symbol, "name");
  const auto version =
      ReadAttribute<std::string>(symbol, "version", std::string());
  const bool is_default_version =
      ReadAttribute<bool>(symbol, "is-default-version", false);
  const auto alias = GetAttribute(symbol, "alias");

  std::string elf_symbol_id = name;
  if (!version.empty()) {
    elf_symbol_id += '@';
    if (is_default_version)
      elf_symbol_id += '@';
    elf_symbol_id += version;
  }

  const SymbolInfo info{name, version, is_default_version, symbol};
  Check(symbol_info_map_.emplace(elf_symbol_id, std::move(info)).second)
      << "multiple symbols with id " << elf_symbol_id;

  if (alias) {
    std::istringstream is(*alias);
    std::string item;
    while (std::getline(is, item, ','))
      Check(alias_to_main_.insert({item, elf_symbol_id}).second)
          << "multiple aliases with id " << elf_symbol_id;
  }
}

bool Abigail::ProcessUserDefinedType(const std::string& name, Id id,
                                     xmlNodePtr decl) {
  if (name == "typedef-decl") {
    ProcessTypedef(id, decl);
  } else if (name == "class-decl") {
    ProcessStructUnion(id, true, decl);
  } else if (name == "union-decl") {
    ProcessStructUnion(id, false, decl);
  } else if (name == "enum-decl") {
    ProcessEnum(id, decl);
  } else {
    return false;
  }
  return true;
}

void Abigail::ProcessScope(xmlNodePtr scope) {
  for (auto element = xmlFirstElementChild(scope); element;
       element = xmlNextElementSibling(element)) {
    const auto name = GetElementName(element);
    const auto type_id = GetAttribute(element, "id");
    // all type elements have "id", all non-types do not
    if (type_id) {
      const auto id = GetNode(*type_id);
      if (graph_.Is(id)) {
        std::cerr << "duplicate definition of type '" << *type_id << "'\n";
        continue;
      }
      if (name == "function-type") {
        ProcessFunctionType(id, element);
      } else if (name == "pointer-type-def") {
        ProcessPointer(id, true, element);
      } else if (name == "reference-type-def") {
        ProcessPointer(id, false, element);
      } else if (name == "qualified-type-def") {
        ProcessQualified(id, element);
      } else if (name == "array-type-def") {
        ProcessArray(id, element);
      } else if (name == "type-decl") {
        ProcessTypeDecl(id, element);
      } else if (!ProcessUserDefinedType(name, id, element)) {
        Die() << "bad abi-instr type child element '" << name << "'";
      }
    } else {
      if (name == "var-decl") {
        ProcessDecl(true, element);
      } else if (name == "function-decl") {
        ProcessDecl(false, element);
      } else if (name == "namespace-decl") {
        ProcessNamespace(element);
      } else {
        Die() << "bad abi-instr non-type child element '" << name << "'";
      }
    }
  }
}

void Abigail::ProcessInstr(xmlNodePtr instr) { ProcessScope(instr); }

void Abigail::ProcessNamespace(xmlNodePtr scope) {
  const auto name = GetAttributeOrDie(scope, "name");
  PushScopeName push_scope_name(scope_name_, name);
  ProcessScope(scope);
}

Id Abigail::ProcessDecl(bool is_variable, xmlNodePtr decl) {
  const auto name = scope_name_ + GetAttributeOrDie(decl, "name");
  const auto mangled_name = GetAttribute(decl, "mangled-name");
  const auto symbol_id = GetAttribute(decl, "elf-symbol-id");
  const auto type = is_variable ? GetEdge(decl)
                                : graph_.Add(MakeFunctionType(decl));
  if (verbose_ && !is_variable)
    std::cerr << type << " function type for function " << name << "\n";
  if (symbol_id) {
    // There's a link to an ELF symbol.
    if (verbose_)
      std::cerr << "ELF symbol " << *symbol_id << " of " << type << "\n";
    const auto [it, inserted] = symbol_id_and_full_name_.emplace(
        *symbol_id, std::make_pair(type, name));
    if (!inserted && it->second.first != type)
      Die() << "conflicting types for '" << *symbol_id << "'";
  }
  return type;
}

void Abigail::ProcessFunctionType(Id id, xmlNodePtr function) {
  graph_.Set(id, MakeFunctionType(function));
}

void Abigail::ProcessTypedef(Id id, xmlNodePtr type_definition) {
  const auto name = scope_name_ + GetAttributeOrDie(type_definition, "name");
  const auto type = GetEdge(type_definition);
  graph_.Set(id, Make<Typedef>(name, type));
  if (verbose_)
    std::cerr << id << " typedef " << name << " of " << type << "\n";
}

void Abigail::ProcessPointer(Id id, bool is_pointer, xmlNodePtr pointer) {
  const auto type = GetEdge(pointer);
  const auto kind = is_pointer ? PointerReference::Kind::POINTER
                               : ReadAttribute<PointerReference::Kind>(
                                     pointer, "kind", &ParseReferenceKind);
  graph_.Set(id, Make<PointerReference>(kind, type));
  if (verbose_)
    std::cerr << id << " " << kind << " to " << type << "\n";
}

void Abigail::ProcessQualified(Id id, xmlNodePtr qualified) {
  std::vector<Qualifier> qualifiers;
  // Do these in reverse order so we get CVR ordering.
  if (ReadAttribute<bool>(qualified, "restrict", false))
    qualifiers.push_back(Qualifier::RESTRICT);
  if (ReadAttribute<bool>(qualified, "volatile", false))
    qualifiers.push_back(Qualifier::VOLATILE);
  if (ReadAttribute<bool>(qualified, "const", false))
    qualifiers.push_back(Qualifier::CONST);
  Check(!qualifiers.empty()) << "qualified-type-def has no qualifiers";
  // Handle multiple qualifiers by unconditionally adding as new nodes all but
  // the last qualifier which is set into place.
  if (verbose_)
    std::cerr << id << " qualified";
  auto type = GetEdge(qualified);
  auto count = qualifiers.size();
  for (auto qualifier : qualifiers) {
    --count;
    auto node = Make<Qualified>(qualifier, type);
    if (count)
      type = graph_.Add(std::move(node));
    else
      graph_.Set(id, std::move(node));
    if (verbose_)
      std::cerr << ' ' << qualifier;
  }
  if (verbose_)
    std::cerr << " of " << id << "\n";
}

void Abigail::ProcessArray(Id id, xmlNodePtr array) {
  std::vector<size_t> dimensions;
  for (auto child = xmlFirstElementChild(array); child;
       child = xmlNextElementSibling(child)) {
    CheckElementName("subrange", child);
    const auto length = ReadAttribute<uint64_t>(child, "length", &ParseLength);
    dimensions.push_back(length);
  }
  Check(!dimensions.empty()) << "array-type-def element has no children";
  // int[M][N] means array[M] of array[N] of int
  //
  // We need to chain a bunch of types together:
  //
  // id = array[n] of id = ... = array[n] of id
  //
  // where the first id is the new type in slot ix
  // and the last id is the old type in slot type
  //
  // Use the same approach as for qualifiers.
  if (verbose_)
    std::cerr << id << " array";
  auto type = GetEdge(array);
  auto count = dimensions.size();
  for (auto it = dimensions.crbegin(); it != dimensions.crend(); ++it) {
    --count;
    const auto size = *it;
    auto node = Make<Array>(size, type);
    if (count)
      type = graph_.Add(std::move(node));
    else
      graph_.Set(id, std::move(node));
    if (verbose_)
      std::cerr << ' ' << size;
  }
  if (verbose_)
    std::cerr << " of " << id << "\n";
}

void Abigail::ProcessTypeDecl(Id id, xmlNodePtr type_decl) {
  const auto name = scope_name_ + GetAttributeOrDie(type_decl, "name");
  const auto bits = ReadAttribute<size_t>(type_decl, "size-in-bits", 0);
  const auto bytes = (bits + 7) / 8;

  if (name == "void") {
    graph_.Set(id, Make<Void>());
  } else if (name == "bool") {
    // TODO: improve terrible INT representation
    graph_.Set(
        id, Make<Integer>(name, Integer::Encoding::BOOLEAN, bits, bytes));
  } else {
    // TODO: What about plain char's signedness?
    bool is_signed = name.find("unsigned") == name.npos;
    bool is_char = name.find("char") != name.npos;
    Integer::Encoding encoding =
        is_char ? is_signed ? Integer::Encoding::SIGNED_CHARACTER
                            : Integer::Encoding::UNSIGNED_CHARACTER
                : is_signed ? Integer::Encoding::SIGNED_INTEGER
                            : Integer::Encoding::UNSIGNED_INTEGER;
    graph_.Set(id, Make<Integer>(name, encoding, bits, bytes));
  }
  if (verbose_)
    std::cerr << id << " " << name << "\n";
}

void Abigail::ProcessStructUnion(Id id, bool is_struct,
                                 xmlNodePtr struct_union) {
  bool forward =
      ReadAttribute<bool>(struct_union, "is-declaration-only", false);
  const auto kind = is_struct
                    ? (ReadAttribute<bool>(struct_union, "is-struct", false)
                           ? StructUnion::Kind::STRUCT
                           : StructUnion::Kind::CLASS)
                    : StructUnion::Kind::UNION;
  const auto name = ReadAttribute<bool>(struct_union, "is-anonymous", false)
                    ? std::string()
                    : GetAttributeOrDie(struct_union, "name");
  const auto full_name = name.empty() ? std::string() : scope_name_ + name;
  PushScopeName push_scope_name(
      scope_name_,
      name.empty() ? (std::ostringstream() << "<unnamed " << kind << ">").str()
                   : name);
  if (forward) {
    graph_.Set(id, Make<StructUnion>(kind, full_name));
    if (verbose_)
      std::cerr << id << " " << kind << " (forward-declared) " << full_name
                << "\n";
    return;
  }
  const auto bits = ReadAttribute<size_t>(struct_union, "size-in-bits", 0);
  const auto bytes = (bits + 7) / 8;

  std::vector<Id> base_classes;
  std::vector<Id> methods;
  std::vector<Id> members;
  for (xmlNodePtr child = xmlFirstElementChild(struct_union); child;
       child = xmlNextElementSibling(child)) {
    const auto child_name = GetElementName(child);
    if (child_name == "data-member") {
      if (const auto member = ProcessDataMember(is_struct, child))
        members.push_back(*member);
    } else if (child_name == "member-type") {
      ProcessMemberType(child);
    } else if (child_name == "base-class") {
      base_classes.push_back(ProcessBaseClass(child));
    } else if (child_name == "member-function") {
      methods.push_back(ProcessMemberFunction(child));
    } else {
      Die() << "unrecognised " << kind << "-decl child element '" << child_name
            << "'";
    }
  }

  graph_.Set(id, Make<StructUnion>(kind, full_name, bytes, base_classes,
                                   methods, members));
  if (verbose_)
    std::cerr << id << " " << kind << " " << full_name << "\n";
}

void Abigail::ProcessEnum(Id id, xmlNodePtr enumeration) {
  bool forward = ReadAttribute<bool>(enumeration, "is-declaration-only", false);
  const auto name = ReadAttribute<bool>(enumeration, "is-anonymous", false)
                    ? std::string()
                    : scope_name_ + GetAttributeOrDie(enumeration, "name");
  if (forward) {
    graph_.Set(id, Make<Enumeration>(name));
    if (verbose_)
      std::cerr << id << " enum (forward-declared) " << name << "\n";
    return;
  }

  xmlNodePtr underlying = xmlFirstElementChild(enumeration);
  Check(underlying) << "enum-decl has no child elements";
  CheckElementName("underlying-type", underlying);
  const auto type = GetEdge(underlying);
  // TODO: decision on underlying type vs size
  (void)type;

  std::vector<std::pair<std::string, int64_t>> enumerators;
  for (xmlNodePtr enumerator = xmlNextElementSibling(underlying); enumerator;
       enumerator = xmlNextElementSibling(enumerator)) {
    CheckElementName("enumerator", enumerator);
    const auto enumerator_name = GetAttributeOrDie(enumerator, "name");
    // libabigail currently supports anything that fits in an int64_t
    const auto enumerator_value =
        ReadAttributeOrDie<int64_t>(enumerator, "value");
    enumerators.emplace_back(enumerator_name, enumerator_value);
  }

  graph_.Set(id, Make<Enumeration>(name, 0, enumerators));
  if (verbose_)
    std::cerr << id << " enum " << name << "\n";
}

Id Abigail::ProcessBaseClass(xmlNodePtr base_class) {
  const auto& type = GetEdge(base_class);
  const auto offset =
      ReadAttributeOrDie<size_t>(base_class, "layout-offset-in-bits");
  const auto inheritance = ReadAttribute<bool>(base_class, "is-virtual", false)
                           ? BaseClass::Inheritance::VIRTUAL
                           : BaseClass::Inheritance::NON_VIRTUAL;
  return graph_.Add(Make<BaseClass>(type, offset, inheritance));
}

std::optional<Id> Abigail::ProcessDataMember(bool is_struct,
                                             xmlNodePtr data_member) {
  xmlNodePtr decl = GetOnlyChild("data-member", data_member);
  CheckElementName("var-decl", decl);
  if (ReadAttribute<bool>(data_member, "static", false)) {
    ProcessDecl(true, decl);
    return {};
  }

  size_t offset = is_struct
              ? ReadAttributeOrDie<size_t>(data_member, "layout-offset-in-bits")
              : 0;
  const auto name = GetAttributeOrDie(decl, "name");
  const auto type = GetEdge(decl);

  // Note: libabigail does not model member size, yet
  return {graph_.Add(Make<Member>(name, type, offset, 0))};
}

Id Abigail::ProcessMemberFunction(xmlNodePtr method) {
  xmlNodePtr decl = GetOnlyChild("member-function", method);
  CheckElementName("function-decl", decl);
  const auto mangled_name = GetAttributeOrDie(decl, "mangled-name");
  const auto name = GetAttributeOrDie(decl, "name");
  const auto type = ProcessDecl(false, decl);
  const auto vtable_offset = ReadAttribute<uint64_t>(method, "vtable-offset");
  const auto kind = vtable_offset
                    ? Method::Kind::VIRTUAL
                    : ReadAttribute<bool>(method, "static", false)
                       ? Method::Kind::STATIC
                       : Method::Kind::NON_VIRTUAL;
  return graph_.Add(
      Make<Method>(mangled_name, name, kind, vtable_offset, type));
}

void Abigail::ProcessMemberType(xmlNodePtr member_type) {
  xmlNodePtr decl = GetOnlyChild("member-type", member_type);
  const auto type_id = GetAttributeOrDie(decl, "id");
  const auto id = GetNode(type_id);
  if (graph_.Is(id)) {
    std::cerr << "duplicate definition of type '" << type_id << "'\n";
    return;
  }
  const auto name = GetElementName(decl);
  if (!ProcessUserDefinedType(name, id, decl))
    Die() << "unrecognised member-type child element '" << name << "'";
}

Id Abigail::BuildSymbol(const SymbolInfo& info,
                        std::optional<Id> type_id,
                        const std::optional<std::string>& name) {
  const xmlNodePtr symbol = info.node;
  const bool is_defined = ReadAttributeOrDie<bool>(symbol, "is-defined");
  const auto crc = ReadAttribute<CRC>(symbol, "crc");
  const auto type = ReadAttributeOrDie<ElfSymbol::SymbolType>(symbol, "type");
  const auto binding =
      ReadAttributeOrDie<ElfSymbol::Binding>(symbol, "binding");
  const auto visibility =
      ReadAttributeOrDie<ElfSymbol::Visibility>(symbol, "visibility");

  return graph_.Add(Make<ElfSymbol>(
      info.name, info.version, info.is_default_version,
      is_defined, type, binding, visibility, crc, type_id, name));
}

Id Abigail::BuildSymbols() {
  // Libabigail's model is (approximately):
  //
  //   (alias)* -> main symbol <- some decl -> type
  //
  // which we turn into:
  //
  //   symbol / alias -> type
  //
  for (const auto& [alias, main] : alias_to_main_)
    Check(!alias_to_main_.count(main))
        << "found main symbol and alias with id " << main;
  // Build final symbol table, tying symbols to their types.
  std::map<std::string, Id> symbols;
  for (const auto& [id, symbol_info] : symbol_info_map_) {
    const auto main = alias_to_main_.find(id);
    const auto lookup = main != alias_to_main_.end() ? main->second : id;
    const auto type_id_and_name_it = symbol_id_and_full_name_.find(lookup);
    std::optional<Id> type_id;
    std::optional<std::string> name;
    if (type_id_and_name_it != symbol_id_and_full_name_.end()) {
      const auto& type_id_and_name = type_id_and_name_it->second;
      type_id = {type_id_and_name.first};
      name = {type_id_and_name.second};
    }
    symbols.insert({id, BuildSymbol(symbol_info, type_id, name)});
  }
  return graph_.Add(Make<Symbols>(symbols));
}

Id Read(Graph& graph, const std::string& path, bool verbose) {
  // Open input for reading.
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    Die() << "could not open '" << path << "' for reading: " << strerror(errno);
  xmlParserCtxtPtr parser_context = xmlNewParserCtxt();

  // Read the XML.
  const auto document =
      std::unique_ptr<std::remove_pointer<xmlDocPtr>::type, void(*)(xmlDocPtr)>(
          xmlCtxtReadFd(parser_context, fd, nullptr, nullptr, 0), xmlFreeDoc);

  // Close input.
  xmlFreeParserCtxt(parser_context);
  close(fd);

  // Get the root element.
  Check(document != nullptr) << "failed to parse input as XML";
  xmlNodePtr root = xmlDocGetRootElement(document.get());
  Check(root) << "XML document has no root element";

  return Abigail(graph, verbose).ProcessRoot(root);
}

}  // namespace abixml
}  // namespace stg
