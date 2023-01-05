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
// Author: Siddharth Nayyar

#ifndef STG_STABLE_IDS_H_
#define STG_STABLE_IDS_H_

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "graph.h"
#include "hashing.h"

namespace stg {

class StableId {
 public:
  StableId(const Graph& graph) : graph_(graph) {}

  uint32_t operator()(Id);
  uint32_t operator()(const Void&);
  uint32_t operator()(const Variadic&);
  uint32_t operator()(const PointerReference&);
  uint32_t operator()(const Typedef&);
  uint32_t operator()(const Qualified&);
  uint32_t operator()(const Primitive&);
  uint32_t operator()(const Array&);
  uint32_t operator()(const BaseClass&);
  uint32_t operator()(const Method&);
  uint32_t operator()(const Member&);
  uint32_t operator()(const StructUnion&);
  uint32_t operator()(const Enumeration&);
  uint32_t operator()(const Function&);
  uint32_t operator()(const ElfSymbol&);
  uint32_t operator()(const Symbols&);

 private:
  const Graph& graph_;
  std::unordered_map<Id, uint32_t> stable_id_cache_;

  // Function object: (Args...) -> uint32_t
  Hash hash_;
};

}  // namespace stg

#endif  // STG_STABLE_IDS_H_
