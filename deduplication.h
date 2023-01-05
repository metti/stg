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
// Author: Giuliano Procida

#ifndef STG_DEDUPLICATION_H_
#define STG_DEDUPLICATION_H_

#include <cstdint>
#include <unordered_map>

#include "graph.h"
#include "metrics.h"

namespace stg {

using Hashes = std::unordered_map<Id, uint32_t>;

Id Deduplicate(Graph& graph, Id root, const Hashes& hashes, Metrics& metrics);

}  // namespace stg

#endif  // STG_DEDUPLICATION_H_
