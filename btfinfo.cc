// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2021 Google LLC
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
// Author: Maria Teguiani
// Author: Giuliano Procida

#include <iostream>

#include "btf_reader.h"
#include "error.h"

int main(int argc, const char* argv[]) {
  if (argc != 2) {
    std::cerr << "Please specify the path to a BTF file.";
    return 1;
  }

  try {
    (void)stg::btf::ReadFile(argv[1], /* verbose = */ true);
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }

  return 0;
}
