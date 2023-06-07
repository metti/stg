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
// Author: Matthias Maennich

#include <stdexcept>

#include <abg-symtab-reader.h>  // for symtab_reader
#include "btf_reader.h"
#include "error.h"

extern "C" int LLVMFuzzerTestOneInput(char* data, size_t size) {
  auto env = std::make_unique<abigail::ir::environment>();

  // Just an empty symtab to satisfy the Structs constructor. The BTF will still
  // be read, just BuildSymbols will exit quickly upon an empty symtab.
  auto symmap = std::make_shared<abigail::ir::string_elf_symbols_map_type>();
  auto symtab = abigail::symtab_reader::symtab::load(symmap, symmap);

  try {
    stg::Graph graph;
    stg::btf::Structs(
        graph, std::move(env), std::move(symtab)).Process(data, size);
  } catch (const stg::Exception&) {
    // Pass as this is us catching invalid BTF properly.
  }
  return 0;
}
