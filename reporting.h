// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2022 Google LLC
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

#ifndef STG_REPORTING_H_
#define STG_REPORTING_H_

#include <ostream>

#include "naming.h"
#include "stg.h"

namespace stg {
namespace reporting {

enum class OutputFormat { PLAIN, FLAT, SMALL, SHORT, VIZ };

struct Options {
  const OutputFormat format;
  const size_t max_crc_only_changes;  // only for SHORT
};

struct Reporting {
  const Graph& graph;
  const Outcomes& outcomes;
  const Options& options;
  NameCache& names;
};

void Report(Reporting& reporting, const Comparison& comparison,
            std::ostream& output);

}  // namespace reporting
}  // namespace stg

#endif  // STG_REPORTING_H_
