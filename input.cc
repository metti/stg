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

#include "input.h"

#include <memory>

#include "abigail_reader.h"
#include "btf_reader.h"
#include "elf_reader.h"
#include "filter.h"
#include "graph.h"
#include "metrics.h"
#include "proto_reader.h"
#include "reader_options.h"

namespace stg {

Id Read(Graph& graph, InputFormat format, const char* input,
        ReadOptions options, const std::unique_ptr<Filter>& file_filter,
        Metrics& metrics) {
  switch (format) {
    case InputFormat::ABI: {
      Time read(metrics, "read ABI");
      return abixml::Read(graph, input, metrics);
    }
    case InputFormat::BTF: {
      Time read(metrics, "read BTF");
      return btf::ReadFile(graph, input, options);
    }
    case InputFormat::ELF: {
      Time read(metrics, "read ELF");
      return elf::Read(graph, input, options, file_filter, metrics);
    }
    case InputFormat::STG: {
      Time read(metrics, "read STG");
      return proto::Read(graph, input);
    }
  }
}

}  // namespace stg
