// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2021-2022 Google LLC
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
// Author: Aleksei Vetrov

#include "elf_reader.h"
#include "error.h"
#include "stg.h"

extern "C" int LLVMFuzzerTestOneInput(char* data, size_t size) {
  try {
    stg::Graph graph;
    stg::elf::Read(graph, data, size, /* verbose= */ false);
  } catch (const stg::Exception&) {
    // Pass as this is us catching invalid ELF properly.
  }
  return 0;
}
