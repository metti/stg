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

#ifndef STG_INPUT_H_
#define STG_INPUT_H_

#include "graph.h"
#include "metrics.h"

namespace stg {

enum class InputFormat { ABI, BTF, ELF, STG };

Id Read(Graph& graph, InputFormat format, const char* input, bool process_dwarf,
        bool info, Metrics& metrics);

}  // namespace stg

#endif  // STG_INPUT_H_
