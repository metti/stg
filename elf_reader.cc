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
// Author: Aleksei Vetrov

#include "elf_reader.h"

#include <iostream>
#include <map>
#include <string>

#include "elf_loader.h"
#include "stg.h"

namespace stg {
namespace elf {

namespace {

Id Read(Graph& graph, elf::ElfLoader&&) {
  std::map<SymbolKey, Id> symbols_map;
  return graph.Add(Make<Symbols>(symbols_map));
}

}  // namespace

Id Read(Graph& graph, const std::string& path, bool verbose) {
  if (verbose)
    std::cout << "Parsing ELF: " << path << '\n';

  return Read(graph, elf::ElfLoader(path));
}

}  // namespace elf
}  // namespace stg
