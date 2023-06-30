// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2023 Google LLC
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
// Author: Aleksei Vetrov

#include <getopt.h>

#include <iostream>
#include <utility>
#include <vector>

#include "input.h"
#include "error.h"
#include "graph.h"
#include "metrics.h"
#include "reader_options.h"

using Input = std::pair<stg::InputFormat, const char*>;

int main(int argc, char* const argv[]) {
  enum LongOptions {
    kSkipDwarf = 256,
  };
  stg::ReadOptions opt_read_options(stg::ReadOptions::INFO);
  static option opts[] = {
      {"btf",        required_argument, nullptr, 'b'       },
      {"elf",        required_argument, nullptr, 'e'       },
      {"skip-dwarf", no_argument,       nullptr, kSkipDwarf},
      {nullptr,      0,                 nullptr, 0         },
  };
  auto usage = [&]() {
    std::cerr << "Parse BTF or ELF with verbose logging.\n"
              << "usage: " << argv[0]
              << " [--skip-dwarf] -b|--btf|-e|--elf file\n";
    return 1;
  };

  std::vector<Input> inputs;
  while (true) {
    int c = getopt_long(argc, argv, "-b:e:", opts, nullptr);
    if (c == -1) {
      break;
    }
    const char* argument = optarg;
    switch (c) {
      case 'b':
        inputs.emplace_back(stg::InputFormat::BTF, argument);
        break;
      case 'e':
        inputs.emplace_back(stg::InputFormat::ELF, argument);
        break;
      case kSkipDwarf:
        opt_read_options.Set(stg::ReadOptions::SKIP_DWARF);
        break;
      default:
        return usage();
    }
  }

  // allow only one input file
  if (inputs.size() != 1) {
    return usage();
  }

  const auto& [format, filename] = inputs[0];

  try {
    stg::Graph graph;
    stg::Metrics metrics;
    (void)stg::Read(graph, format, filename, opt_read_options, metrics);
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }

  return 0;
}
