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

std::optional<Ptr::Kind> ParseReferenceKind(const std::string& value) {
  if (value == "lvalue")
    return {Ptr::Kind::LVALUE_REFERENCE};
  if (value == "rvalue")
    return {Ptr::Kind::RVALUE_REFERENCE};
  return {};
}

}  // namespace

Abigail::Abigail(Graph& graph, bool verbose)
    : graph_(graph), verbose_(verbose),
      env_(std::make_unique<abigail::ir::environment>()) { }

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

std::unique_ptr<Type> Abigail::MakeFunctionType(xmlNodePtr function) {
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
        Parameter parameter{.name_ = std::string(), .typeId_ = type};
        parameters.push_back(std::move(parameter));
      } else {
        const auto name = GetAttribute(child, "name");
        const auto type = GetEdge(child);
        Parameter parameter{.name_ = name ? *name : std::string(),
                            .typeId_ = type};
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
      std::cerr << p.typeId_;
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
  const auto name = GetAttributeOrDie(symbol, "name");
  const auto size = ReadAttribute<size_t>(symbol, "size", 0);
  const bool is_defined = ReadAttributeOrDie<bool>(symbol, "is-defined");
  const bool is_common = ReadAttribute<bool>(symbol, "is-common", false);
  const auto version =
      ReadAttribute<std::string>(symbol, "version", std::string());
  const bool is_default_version =
      ReadAttribute<bool>(symbol, "is-default-version", false);
  const auto crc = ReadAttribute<CRC>(symbol, "crc", CRC{0});
  const auto type = GetAttributeOrDie(symbol, "type");
  const auto binding = GetAttributeOrDie(symbol, "binding");
  const auto visibility = GetAttributeOrDie(symbol, "visibility");
  const auto alias = GetAttribute(symbol, "alias");

  abigail::elf_symbol::type sym_type;
  Check(string_to_elf_symbol_type(type, sym_type))
      << "unrecognised elf-symbol type '" << type << "'";

  abigail::elf_symbol::binding sym_binding;
  Check(string_to_elf_symbol_binding(binding, sym_binding))
      << "unrecognised elf-symbol binding '" << binding << "'";

  abigail::elf_symbol::visibility sym_visibility;
  Check(string_to_elf_symbol_visibility(visibility, sym_visibility))
      << "unrecognised elf-symbol visibility '" << visibility << "'";

  const auto sym_version =
      abigail::elf_symbol::version(version, is_default_version);

  std::vector<std::string> aliases;
  if (alias) {
    std::istringstream is(*alias);
    std::string item;
    while (std::getline(is, item, ','))
      aliases.push_back(item);
  }

  auto elf_symbol = abigail::elf_symbol::create(
      env_.get(), /*index=*/0, size, name, sym_type, sym_binding, is_defined,
      is_common, sym_version, sym_visibility);

  if (crc.number)
    elf_symbol->set_crc(crc.number);

  elf_symbol_aliases_.push_back({elf_symbol, aliases});
}

void Abigail::ProcessInstr(xmlNodePtr instr) {
  for (auto element = xmlFirstElementChild(instr); element;
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
      } else if (name == "typedef-decl") {
        ProcessTypedef(id, element);
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
      } else if (name == "class-decl") {
        ProcessStructUnion(id, true, element);
      } else if (name == "union-decl") {
        ProcessStructUnion(id, false, element);
      } else if (name == "enum-decl") {
        ProcessEnum(id, element);
      } else {
        Die() << "bad abi-instr type child element '" << name << "'";
      }
    } else {
      if (name == "var-decl") {
        ProcessDecl(true, element);
      } else if (name == "function-decl") {
        ProcessDecl(false, element);
      } else {
        Die() << "bad abi-instr non-type child element '" << name << "'";
      }
    }
  }
}

void Abigail::ProcessDecl(bool is_variable, xmlNodePtr decl) {
  const auto name = GetAttributeOrDie(decl, "name");
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
}

void Abigail::ProcessFunctionType(Id id, xmlNodePtr function) {
  graph_.Set(id, MakeFunctionType(function));
}

void Abigail::ProcessTypedef(Id id, xmlNodePtr type_definition) {
  const auto name = GetAttributeOrDie(type_definition, "name");
  const auto type = GetEdge(type_definition);
  graph_.Set(id, Make<Typedef>(name, type));
  if (verbose_)
    std::cerr << id << " typedef " << name << " of " << type << "\n";
}

void Abigail::ProcessPointer(Id id, bool isPointer, xmlNodePtr pointer) {
  const auto type = GetEdge(pointer);
  const auto kind = isPointer
              ? Ptr::Kind::POINTER
              : ReadAttribute<Ptr::Kind>(pointer, "kind", &ParseReferenceKind);
  graph_.Set(id, Make<Ptr>(kind, type));
  if (verbose_)
    std::cerr << id << " " << kind << " to " << type << "\n";
}

void Abigail::ProcessQualified(Id id, xmlNodePtr qualified) {
  std::vector<QualifierKind> qualifiers;
  // Do these in reverse order so we get CVR ordering.
  if (ReadAttribute<bool>(qualified, "restrict", false))
    qualifiers.push_back(QualifierKind::RESTRICT);
  if (ReadAttribute<bool>(qualified, "volatile", false))
    qualifiers.push_back(QualifierKind::VOLATILE);
  if (ReadAttribute<bool>(qualified, "const", false))
    qualifiers.push_back(QualifierKind::CONST);
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
    auto node = Make<Array>(type, size);
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
  const auto name = GetAttributeOrDie(type_decl, "name");
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

void Abigail::ProcessStructUnion(
    Id id, bool is_struct, xmlNodePtr struct_union) {
  bool forward =
      ReadAttribute<bool>(struct_union, "is-declaration-only", false);
  const auto name = ReadAttribute<bool>(struct_union, "is-anonymous", false)
                    ? std::string()
                    : GetAttributeOrDie(struct_union, "name");
  const auto kind = is_struct ? StructUnionKind::STRUCT
                              : StructUnionKind::UNION;
  if (forward) {
    graph_.Set(id, Make<StructUnion>(name, kind));
    if (verbose_)
      std::cerr << id << " " << kind << " (forward-declared) " << name << "\n";
    return;
  }
  const auto bits = ReadAttribute<size_t>(struct_union, "size-in-bits", 0);
  const auto bytes = (bits + 7) / 8;

  std::vector<Id> members;
  for (xmlNodePtr child = xmlFirstElementChild(struct_union); child;
       child = xmlNextElementSibling(child)) {
    CheckElementName("data-member", child);
    size_t offset = is_struct
                    ? ReadAttributeOrDie<size_t>(child, "layout-offset-in-bits")
                    : 0;
    xmlNodePtr decl = xmlFirstElementChild(child);
    Check(decl && !xmlNextElementSibling(decl))
        << "data-member with not exactly one child element";
    CheckElementName("var-decl", decl);
    const auto member_name = GetAttributeOrDie(decl, "name");
    const auto type = GetEdge(decl);
    // Note: libabigail does not model member size, yet
    members.push_back(graph_.Add(Make<Member>(member_name, type, offset, 0)));
  }

  graph_.Set(id, Make<StructUnion>(name, kind, bytes, members));
  if (verbose_)
    std::cerr << id << " " << kind << " " << name << "\n";
}

void Abigail::ProcessEnum(Id id, xmlNodePtr enumeration) {
  bool forward = ReadAttribute<bool>(enumeration, "is-declaration-only", false);
  const auto name = ReadAttribute<bool>(enumeration, "is-anonymous", false)
                    ? std::string()
                    : GetAttributeOrDie(enumeration, "name");
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

Id Abigail::BuildSymbols() {
  // Libabigail's model is (approximately):
  //
  //   (alias)* -> main symbol <- some decl -> type
  //
  // which we turn into:
  //
  //   symbol / alias -> type
  //
  // Make some auxiliary maps.
  std::unordered_map<std::string, abigail::elf_symbol_sptr> id_to_symbol;
  std::unordered_map<std::string, std::string> alias_to_main;
  for (auto& [symbol, aliases] : elf_symbol_aliases_) {
    const auto id = symbol->get_id_string();
    Check(id_to_symbol.insert({id, symbol}).second)
        << "multiple symbols with id " << id;
    for (const auto& alias : aliases)
      Check(alias_to_main.insert({alias, id}).second)
          << "multiple aliases with id " << alias;
  }
  for (const auto& [alias, main] : alias_to_main)
    Check(!alias_to_main.count(main))
        << "found main symbol and alias with id " << main;
  // Tie aliases to their main symbol.
  for (const auto& [alias, main] : alias_to_main) {
    const auto it = id_to_symbol.find(alias);
    Check(it != id_to_symbol.end())
        << "missing symbol alias " << alias;
    id_to_symbol[main]->add_alias(it->second);
  }
  // Build final symbol table, tying symbols to their types.
  std::map<std::string, Id> symbols;
  for (const auto& [id, symbol] : id_to_symbol) {
    const auto main = alias_to_main.find(id);
    const auto lookup = main != alias_to_main.end() ? main->second : id;
    const auto it = symbol_id_and_full_name_.find(lookup);
    std::optional<Id> type_id;
    std::optional<std::string> name;
    if (it != symbol_id_and_full_name_.end()) {
      type_id = {it->second.first};
      name = {it->second.second};
    }
    symbols.insert({id, graph_.Add(Make<ElfSymbol>(symbol, type_id, name))});
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
