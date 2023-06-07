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
// Author: Maria Teguiani
// Author: Giuliano Procida
// Author: Aleksei Vetrov

#include <getopt.h>

#include <iostream>
#include <string>

#include "abigail_reader.h"
#include "elf_reader.h"
#include "btf_reader.h"
#include "error.h"

enum class InputFormat { BTF, ELF };
using Input = std::pair<InputFormat, std::string>;

int main(int argc, char* const argv[]) {
  static option opts[] = {
      {"btf",    required_argument, nullptr, 'b'},
      {"elf",    required_argument, nullptr, 'e'},
      {nullptr,  0,                 nullptr, 0  },
  };
  auto usage = [&]() {
    std::cerr << "Parse BTF or ELF with verbose logging.\n"
              << "usage: " << argv[0] << " -b|--btf|-e|--elf file\n";
    return 1;
  };

  std::optional<Input> input;
  while (true) {
    int c = getopt_long(argc, argv, "-b:e:", opts, nullptr);
    if (c == -1)
      break;
    const char* argument = optarg;
    InputFormat input_format;
    switch (c) {
      case 'b':
        input_format = InputFormat::BTF;
        break;
      case 'e':
        input_format = InputFormat::ELF;
        break;
      default:
        return usage();
    }
    // allow only one input file
    if (input)
      return usage();
    input.emplace(input_format, argument);
  }

  if (!input)
    return usage();

  const auto& [format, filename] = *input;

  try {
    stg::Graph graph;
    switch (format) {
      case InputFormat::BTF: {
        (void)stg::btf::ReadFile(graph, filename, /* verbose = */ true);
        break;
      }
      case InputFormat::ELF: {
        (void)stg::elf::Read(graph, filename, /* verbose = */ true);
        break;
      }
    }
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }

  return 0;
}
