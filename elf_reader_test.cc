// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2023 Google LLC
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

#include <catch2/catch.hpp>

namespace Test {

TEST_CASE("GetKsymtabSymbols") {
  const stg::elf::internal::SymbolTable all_symbols = {
      {.name = "foo"},
      {.name = "__ksymtab_foo"},
      {.name = "bar"},
  };
  const auto ksymtab = stg::elf::internal::GetKsymtabSymbols(all_symbols);
  REQUIRE(ksymtab.size() == 1);
  CHECK(*ksymtab.begin() == "foo");
}

}  // namespace Test
