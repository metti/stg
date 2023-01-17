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

#ifndef STG_DWARF_PROCESSOR_H_
#define STG_DWARF_PROCESSOR_H_

#include <vector>

#include "dwarf_wrappers.h"
#include "graph.h"

namespace stg {
namespace dwarf {

struct Types {
  struct Symbol {
    std::string name;
    std::optional<std::string> linkage_name;
    std::optional<size_t> address;
    Id id;
  };

  size_t processed_entries = 0;
  // Container for all Ids allocated during DWARF processing.
  std::vector<Id> all_ids;
  std::vector<Symbol> symbols;
};

// Process every compilation unit from DWARF and returns processed STG along
// with information needed for matching to ELF symbols.
Types Process(Handler& dwarf, Graph& graph);

// Process every entry passed there. It should be used only for testing.
Types ProcessEntries(std::vector<Entry> entries);

}  // namespace dwarf
}  // namespace stg

#endif  // STG_DWARF_PROCESSOR_H_
