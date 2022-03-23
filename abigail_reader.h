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

#ifndef STG_ABIGAIL_READER_H_
#define STG_ABIGAIL_READER_H_

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "id.h"
#include "stg.h"
#include <libxml/tree.h>

namespace stg {
namespace abixml {

// Parser for libabigail's ABI XML format, creating a Symbol-Type Graph.
//
// On construction Abigail consumes a libxml node tree and builds a graph. If
// verbose is set, it gives a running account on stderr of the graph nodes
// created.
//
// The parser supports C types only, with C++ types to be added later.
//
// The main producer of ABI XML is abidw. The format has no formal specification
// and has very limited semantic versioning. This parser makes no attempt to
// support or correct for deficiencies in older versions of the format.
//
// The parser detects unexpected elements and will abort on the presence of at
// least: namespace, base class and member function information.
//
// The parser ignores attributes it doesn't care about, including member access
// specifiers and (meaningless) type ids on array dimensions.
//
// The STG IR and libabigail ABI XML models diverge in some ways. The parser has
// to do extra work for each of these, as follows.
//
// 0. XML uses type and symbol ids to link together elements. These become edges
// in the graph between symbols and types and between types and types. Dangling
// type references will cause an abort. libabigail is much more relaxed about
// symbols without type information and these are modelled as such.
//
// 1. XML function declarations have in-line types. The parser creates
// free-standing types on-the-fly. A useful space optimisation might be to
// prevent duplicate creation of such types.
//
// 2. Variadic parameters are currently flagged with an XML attribute. A
// variadic type node is created on demand and will be shared by all such
// paramerters.
//
// 3. XML symbols and aliases have a rather poor repesentation with aliases
// represented as comma-separated attribute values. Aliases are resolved in a
// post-processing phase.
//
// 4. XML anonymous types also have unhelpful names, these are ignored.
class Abigail : public Graph {
 public:
  explicit Abigail(xmlNodePtr root, bool verbose = false);
  Id Root() const final { return root_; }

 private:
  const bool verbose_;

  // The STG IR uses a distinct node type for the variadic parameter type; if
  // allocated, this is its STG node id.
  std::optional<Id> variadic_;
  // Map from libabigail type ids to STG node ids; except for the type of
  // variadic parameters.
  std::unordered_map<std::string, Id> type_ids_;

  std::unique_ptr<abigail::ir::environment> env_;
  std::vector<std::pair<abigail::elf_symbol_sptr, std::vector<std::string>>>
      elf_symbol_aliases_;
  // libabigail decorates certain declarations with symbol ids; this is the
  // mapping from symbol id to the corresponding type.
  std::unordered_map<std::string, Id> symbol_ids_;
  Id root_ = Id(-1);

  Id GetNode(const std::string& type_id);
  Id GetEdge(xmlNodePtr element);
  Id GetVariadic();
  std::unique_ptr<Type> MakeFunctionType(xmlNodePtr function);

  Id ProcessRoot(xmlNodePtr root);
  void ProcessCorpusGroup(xmlNodePtr group);
  void ProcessCorpus(xmlNodePtr corpus);
  void ProcessSymbols(xmlNodePtr symbols);
  void ProcessSymbol(xmlNodePtr symbol);
  void ProcessInstr(xmlNodePtr instr);

  void ProcessDecl(bool is_variable, xmlNodePtr decl);

  void ProcessFunctionType(Id id, xmlNodePtr function);
  void ProcessTypedef(Id id, xmlNodePtr type_definition);
  void ProcessPointer(Id id, xmlNodePtr pointer);
  void ProcessQualified(Id id, xmlNodePtr qualified);
  void ProcessArray(Id id, xmlNodePtr array);
  void ProcessTypeDecl(Id id, xmlNodePtr type_decl);
  void ProcessStructUnion(Id id, bool is_struct, xmlNodePtr struct_union);
  void ProcessEnum(Id id, xmlNodePtr enumeration);

  Id BuildSymbols();
};

std::unique_ptr<Abigail> Read(const std::string& path, bool verbose = false);

}  // namespace abixml
}  // namespace stg

#endif  // STG_ABIGAIL_READER_H_
