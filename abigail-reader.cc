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

#include "abigail-reader.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <type_traits>

#include <libxml/parser.h>

namespace stg {
namespace abixml {

namespace {

#ifdef FOR_FUZZING
#define exit(n) throw AbigailReaderException()
#endif

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
  if (strcmp(element_name, name) != 0) {
    std::cerr << "expected element '" << name
              << "' but got '" << element_name << "'\n";
    exit(1);
  }
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
  if (attribute) {
    std::string result = FromLibxml(attribute);
    xmlFree(attribute);
    return result;
  }
  std::cerr << "element '" << FromLibxml(node->name)
            << "' missing attribute '" << name << "'\n";
  exit(1);
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

struct CRC {
  uint64_t number;
};

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
  if (!parse) {
    std::cerr << "element '" << FromLibxml(element->name)
              << "' has attribute '" << name
              << "' with bad value '" << value << "'\n";
    exit(1);
  }
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

}  // namespace

Abigail::Abigail(xmlNodePtr root, bool verbose)
    : verbose_(verbose), env_(std::make_unique<abigail::ir::environment>()) {
  ProcessRoot(root);
  BuildSymbols();
}

Id Abigail::Add(std::unique_ptr<Type> type) {
  const auto ix = types_.size();
  types_.push_back(std::move(type));
  return Id(ix);
}

size_t Abigail::GetIndex(const std::string& type_id) {
  const auto [it, inserted] = type_ids_.insert({type_id, types_.size()});
  if (inserted)
    types_.push_back(nullptr);
  return it->second;
}

Id Abigail::GetTypeId(xmlNodePtr element) {
  return Id(GetIndex(GetAttributeOrDie(element, "type-id")));
}

Id Abigail::GetVariadicId() {
  if (!variadic_type_id_) {
    variadic_type_id_ = Add(std::make_unique<Variadic>(types_));
    if (verbose_)
      std::cerr << *variadic_type_id_ << " variadic parameter\n";
  }
  return *variadic_type_id_;
}

std::unique_ptr<Function> Abigail::MakeFunctionType(xmlNodePtr function) {
  std::vector<Parameter> parameters;
  std::optional<Id> return_type;
  for (auto child = xmlFirstElementChild(function); child;
       child = xmlNextElementSibling(child)) {
    const auto child_name = GetElementName(child);
    if (return_type) {
      std::cerr << "unexpected element after return-type\n";
      exit(1);
    }
    if (child_name == "parameter") {
      const auto is_variadic = ReadAttribute<bool>(child, "is-variadic", false);
      if (is_variadic) {
        const auto type = GetVariadicId();
        Parameter parameter{.name_ = std::string(), .typeId_ = type};
        parameters.push_back(std::move(parameter));
      } else {
        const auto name = GetAttribute(child, "name");
        const auto type = GetTypeId(child);
        Parameter parameter{.name_ = name ? *name : std::string(),
                            .typeId_ = type};
        parameters.push_back(std::move(parameter));
      }
    } else if (child_name == "return") {
      return_type = {GetTypeId(child)};
    } else {
      std::cerr << "unrecognised function-decl child element '" << child_name
                << "'\n";
    }
  }
  if (!return_type) {
    std::cerr << "missing return-type\n";
    exit(1);
  }
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
  return std::make_unique<Function>(types_, *return_type, parameters);
}

void Abigail::ProcessRoot(xmlNodePtr root) {
  const auto name = GetElementName(root);
  if (name == "abi-corpus-group") {
    ProcessCorpusGroup(root);
  } else if (name == "abi-corpus") {
    ProcessCorpus(root);
  } else {
    std::cerr << "unrecognised root element '" << name << "'\n";
    exit(1);
  }
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
    } else if (name == "abi-instr") {
      ProcessInstr(element);
    } else {
      std::cerr << "unrecognised abi-corpus child element '" << name << "'\n";
      exit(1);
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
  const auto name = ReadAttribute<std::string>(symbol, "name", std::string());
  const auto size = ReadAttribute<size_t>(symbol, "size", 0);
  const bool is_defined = ReadAttribute<bool>(symbol, "is-defined", true);
  const bool is_common = ReadAttribute<bool>(symbol, "is-common", false);
  const auto version =
      ReadAttribute<std::string>(symbol, "version", std::string());
  const bool is_default_version =
      ReadAttribute<bool>(symbol, "is-default-version", false);
  const auto crc = ReadAttribute<CRC>(symbol, "crc", CRC{0});
  const auto type = GetAttribute(symbol, "type");
  const auto binding = GetAttribute(symbol, "binding");
  const auto visibility = GetAttribute(symbol, "visibility");
  const auto alias = GetAttribute(symbol, "alias");

  auto sym_type = abigail::elf_symbol::NOTYPE_TYPE;
  if (type)
    string_to_elf_symbol_type(*type, sym_type);

  auto sym_binding = abigail::elf_symbol::GLOBAL_BINDING;
  if (binding)
    string_to_elf_symbol_binding(*binding, sym_binding);

  auto sym_visibility = abigail::elf_symbol::DEFAULT_VISIBILITY;
  if (visibility)
    string_to_elf_symbol_visibility(*visibility, sym_visibility);

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
    const auto id = GetAttribute(element, "id");
    // all type elements have "id", all non-types do not
    if (id) {
      const auto ix = GetIndex(*id);
      if (types_[ix]) {
        std::cerr << "duplicate definition of type '" << *id << "'\n";
        continue;
      }
      if (name == "function-type") {
        ProcessFunctionType(ix, element);
      } else if (name == "typedef-decl") {
        ProcessTypedef(ix, element);
      } else if (name == "pointer-type-def") {
        ProcessPointer(ix, element);
      } else if (name == "qualified-type-def") {
        ProcessQualified(ix, element);
      } else if (name == "array-type-def") {
        ProcessArray(ix, element);
      } else if (name == "type-decl") {
        ProcessTypeDecl(ix, element);
      } else if (name == "class-decl") {
        ProcessStructUnion(ix, true, element);
      } else if (name == "union-decl") {
        ProcessStructUnion(ix, false, element);
      } else if (name == "enum-decl") {
        ProcessEnum(ix, element);
      } else {
        std::cerr << "bad abi-instr type child element '" << name << "'\n";
        exit(1);
      }
    } else {
      if (name == "var-decl") {
        ProcessDecl(true, element);
      } else if (name == "function-decl") {
        ProcessDecl(false, element);
      } else {
        std::cerr << "bad abi-instr non-type child element '" << name << "'\n";
        exit(1);
      }
    }
  }
}

void Abigail::ProcessDecl(bool is_variable, xmlNodePtr decl) {
  const auto name = GetAttributeOrDie(decl, "name");
  const auto mangled_name = GetAttribute(decl, "mangled-name");
  const auto symbol_id = GetAttribute(decl, "elf-symbol-id");
  const auto type = is_variable ? GetTypeId(decl) : Add(MakeFunctionType(decl));
  if (verbose_ && !is_variable)
    std::cerr << Id(type) << " function type for function " << name << "\n";
  if (symbol_id) {
    // There's a link to an ELF symbol.
    if (verbose_)
      std::cerr << "ELF symbol " << *symbol_id << " of " << type << "\n";
    const auto [it, inserted] = symbol_id_to_type_.emplace(*symbol_id, type);
    if (!inserted && it->second.ix_ != type.ix_) {
      std::cerr << "conflicting types for '" << *symbol_id << "'\n";
      exit(1);
    }
  }
}

void Abigail::ProcessFunctionType(size_t ix, xmlNodePtr function) {
  types_[ix] = MakeFunctionType(function);
}

void Abigail::ProcessTypedef(size_t ix, xmlNodePtr type_definition) {
  const auto name = GetAttributeOrDie(type_definition, "name");
  const auto type = GetTypeId(type_definition);
  types_[ix] = std::make_unique<Typedef>(types_, name, type);
  if (verbose_)
    std::cerr << Id(ix) << " typedef " << name << " of " << type << "\n";
}

void Abigail::ProcessPointer(size_t ix, xmlNodePtr pointer) {
  const auto type = GetTypeId(pointer);
  types_[ix] = std::make_unique<Ptr>(types_, type);
  if (verbose_)
    std::cerr << Id(ix) << " pointer to " << type << "\n";
}

void Abigail::ProcessQualified(size_t ix, xmlNodePtr qualified) {
  std::vector<QualifierKind> qualifiers;
  // Do these in reverse order so we get CVR ordering.
  if (ReadAttribute<bool>(qualified, "restrict", false))
    qualifiers.push_back(QualifierKind::RESTRICT);
  if (ReadAttribute<bool>(qualified, "volatile", false))
    qualifiers.push_back(QualifierKind::VOLATILE);
  if (ReadAttribute<bool>(qualified, "const", false))
    qualifiers.push_back(QualifierKind::CONST);
  if (qualifiers.empty()) {
    std::cerr << "qualified-type-def has no qualifiers\n";
    exit(1);
  }
  // Handle multiple qualifiers by unconditionally adding the qualifiers as new
  // nodes and the swapping the last one into place.
  if (verbose_)
    std::cerr << Id(ix) << " qualified";
  auto type = GetTypeId(qualified);
  for (auto qualifier : qualifiers) {
    type = Add(std::make_unique<Qualifier>(types_, qualifier, type));
    if (verbose_)
      std::cerr << ' ' << qualifier;
  }
  std::swap(types_[ix], types_.back());
  types_.pop_back();
  if (verbose_)
    std::cerr << " of " << type << "\n";
}

void Abigail::ProcessArray(size_t ix, xmlNodePtr array) {
  std::vector<size_t> dimensions;
  for (auto child = xmlFirstElementChild(array); child;
       child = xmlNextElementSibling(child)) {
    CheckElementName("subrange", child);
    const auto length = ReadAttribute<uint64_t>(child, "length", &ParseLength);
    dimensions.push_back(length);
  }
  if (dimensions.empty()) {
    std::cerr << "array-type-def element has no children\n";
    exit(1);
  }
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
    std::cerr << Id(ix) << " array";
  auto type = GetTypeId(array);
  for (auto size : dimensions) {
    type = Add(std::make_unique<Array>(types_, type, size));
    if (verbose_)
      std::cerr << ' ' << size;
  }
  std::swap(types_[ix], types_.back());
  types_.pop_back();
  if (verbose_)
    std::cerr << " of " << type << "\n";
}

void Abigail::ProcessTypeDecl(size_t ix, xmlNodePtr type_decl) {
  const auto name = GetAttributeOrDie(type_decl, "name");
  const auto bits = ReadAttribute<size_t>(type_decl, "size-in-bits", 0);
  const auto bytes = (bits + 7) / 8;

  if (name == "void") {
    types_[ix] = std::make_unique<Void>(types_);
  } else if (name == "bool") {
    // TODO: improve terrible INT representation
    types_[ix] = std::make_unique<Integer>(
        types_, name, Integer::Encoding::BOOLEAN, bits, bytes);
  } else {
    // TODO: What about plain char's signedness?
    bool is_signed = name.find("unsigned") == name.npos;
    bool is_char = name.find("char") != name.npos;
    Integer::Encoding encoding =
        is_char ? is_signed ? Integer::Encoding::SIGNED_CHARACTER
                            : Integer::Encoding::UNSIGNED_CHARACTER
                : is_signed ? Integer::Encoding::SIGNED_INTEGER
                            : Integer::Encoding::UNSIGNED_INTEGER;
    types_[ix] = std::make_unique<Integer>(types_, name, encoding, bits, bytes);
  }
  if (verbose_)
    std::cerr << Id(ix) << " " << name << "\n";
}

void Abigail::ProcessStructUnion(
    size_t ix, bool is_struct, xmlNodePtr struct_union) {
  bool forward =
      ReadAttribute<bool>(struct_union, "is-declaration-only", false);
  const auto name = ReadAttribute<bool>(struct_union, "is-anonymous", false)
                    ? std::string()
                    : GetAttributeOrDie(struct_union, "name");
  if (forward) {
    const auto kind = is_struct ? ForwardDeclarationKind::STRUCT
                                : ForwardDeclarationKind::UNION;
    types_[ix] = std::make_unique<ForwardDeclaration>(types_, name, kind);
    if (verbose_)
      std::cerr << Id(ix) << " forward " << kind << " " << name << "\n";
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
    if (!decl || xmlNextElementSibling(decl)) {
      std::cerr << "data-member with not exactly one child element\n";
      exit(1);
    }
    CheckElementName("var-decl", decl);
    const auto member_name = GetAttributeOrDie(decl, "name");
    const auto type = GetTypeId(decl);
    // Note: libabigail does not model member size, yet
    auto member =
        Add(std::make_unique<Member>(types_, member_name, type, offset, 0));
    members.push_back(member);
  }

  const auto kind = is_struct ? StructUnionKind::STRUCT
                              : StructUnionKind::UNION;
  types_[ix] =
      std::make_unique<StructUnion>(types_, name, kind, bytes, members);
  if (verbose_)
    std::cerr << Id(ix) << " " << kind << " " << name << "\n";
}

void Abigail::ProcessEnum(size_t ix, xmlNodePtr enumeration) {
  bool forward = ReadAttribute<bool>(enumeration, "is-declaration-only", false);
  const auto name = ReadAttribute<bool>(enumeration, "is-anonymous", false)
                    ? std::string()
                    : GetAttributeOrDie(enumeration, "name");
  if (forward) {
    const auto kind = ForwardDeclarationKind::ENUM;
    types_[ix] = std::make_unique<ForwardDeclaration>(types_, name, kind);
    if (verbose_)
      std::cerr << Id(ix) << " forward " << kind << " " << name << "\n";
    return;
  }

  xmlNodePtr underlying = xmlFirstElementChild(enumeration);
  if (!underlying) {
    std::cerr << "enum-decl has no child elements\n";
    exit(1);
  }
  CheckElementName("underlying-type", underlying);
  const auto type = GetTypeId(underlying);
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

  types_[ix] = std::make_unique<Enumeration>(types_, name, 0, enumerators);
  if (verbose_)
    std::cerr << Id(ix) << " enum " << name << "\n";
}

void Abigail::BuildSymbols() {
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
    if (!id_to_symbol.insert({id, symbol}).second) {
      std::cerr << "multiple symbols with id " << id << '\n';
      exit(1);
    }
    for (const auto& alias : aliases) {
      if (!alias_to_main.insert({alias, id}).second) {
        std::cerr << "multiple aliases with id " << alias << '\n';
        exit(1);
      }
    }
  }
  for (auto& [alias, main] : alias_to_main) {
    if (alias_to_main.count(main)) {
      std::cerr << "found main symbol and alias with id " << main << '\n';
      exit(1);
    }
  }
  // Tie aliases to their main symbol.
  for (const auto& [alias, main] : alias_to_main) {
    const auto it = id_to_symbol.find(alias);
    if (it == id_to_symbol.end()) {
      std::cerr << "missing symbol alias " << alias << '\n';
      exit(1);
    }
    id_to_symbol[main]->add_alias(it->second);
  }
  // Build final symbol table, tying symbols to their types.
  std::map<std::string, Id> symbols;
  for (const auto& [id, symbol] : id_to_symbol) {
    const auto main = alias_to_main.find(id);
    const auto lookup = main != alias_to_main.end() ? main->second : id;
    const auto it = symbol_id_to_type_.find(lookup);
    std::optional<Id> type_id;
    if (it != symbol_id_to_type_.end())
      type_id = {it->second};
    const auto ix = types_.size();
    types_.push_back(std::make_unique<ElfSymbol>(types_, symbol, type_id));
    symbols.insert({id, Id(ix)});
  }
  symbols_index_ = types_.size();
  types_.push_back(std::make_unique<Symbols>(types_, symbols));
}

std::unique_ptr<Abigail> Read(const std::string& path, bool verbose) {
  // Open input for reading.
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    std::cerr << "could not open '" << path << "' for reading: "
              << strerror(errno) << '\n';
    exit(1);
  }
  xmlParserCtxtPtr parser_context = xmlNewParserCtxt();

  // Read the XML.
  const auto document =
      std::unique_ptr<std::remove_pointer<xmlDocPtr>::type, void(*)(xmlDocPtr)>(
          xmlCtxtReadFd(parser_context, fd, nullptr, nullptr, 0), xmlFreeDoc);

  // Close input.
  xmlFreeParserCtxt(parser_context);
  close(fd);

  // Get the root element.
  if (!document) {
    std::cerr << "failed to parse input as XML\n";
    exit(1);
  }
  xmlNodePtr root = xmlDocGetRootElement(document.get());
  if (!root) {
    std::cerr << "XML document has no root element\n";
    exit(1);
  }

  return std::make_unique<Abigail>(root, verbose);
}

}  // namespace abixml
}  // namespace stg
