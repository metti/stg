// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2022-2023 Google LLC
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

#ifndef STG_SYMBOL_FILTER_H_
#define STG_SYMBOL_FILTER_H_

#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>

#include "error.h"

namespace stg {

using SymbolSet = std::unordered_set<std::string>;

// Abstract base class for filtering symbols.
class SymbolFilter {
 public:
  virtual ~SymbolFilter() = default;
  // Filter predicate evaluation.
  virtual bool operator()(const std::string& symbol) const = 0;
};

// Tokenise and parse a symbol filter expression.
std::unique_ptr<SymbolFilter> MakeSymbolFilter(const std::string& filter);

void SymbolFilterUsage(std::ostream& os);

}  // namespace stg

#endif  // STG_SYMBOL_FILTER_H_
