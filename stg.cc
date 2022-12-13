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

#include <getopt.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "abigail_reader.h"
#include "btf_reader.h"
#include "elf_reader.h"
#include "error.h"
#include "graph.h"
#include "timing.h"

namespace stg {
namespace {

Times times;

enum class InputFormat { ABI, BTF, ELF };

Id Read(Graph& graph, InputFormat format, const char* input, bool info) {
  switch (format) {
    case InputFormat::ABI: {
      Time read(times, "read ABI");
      return abixml::Read(graph, input, info);
    }
    case InputFormat::BTF: {
      Time read(times, "read BTF");
      return btf::ReadFile(graph, input, info);
    }
    case InputFormat::ELF: {
      Time read(times, "read ELF");
      return elf::Read(graph, input, info);
    }
  }
}

struct GetSymbols {
  const Symbols& operator()(const Symbols& x) {
    return x;
  }

  template <typename Node>
  const Symbols& operator()(const Node&) {
    Die() << "merge can only merge Symbols nodes";
  }
};

Id Merge(Graph& graph, const std::vector<Id>& roots) {
  if (roots.size() == 1) {
    return roots[0];
  }
  std::map<std::string, Id> symbols;
  GetSymbols get;
  for (auto root : roots) {
    for (const auto& x : graph.Apply<const Symbols&>(get, root).symbols) {
      if (!symbols.insert(x).second) {
        Die() << "merge failed with duplicate symbol: " << x.first;
      }
    }
  }
  return graph.Add<Symbols>(symbols);
}

void Write(const Graph& graph, Id root, const char* output,
           bool stable, bool counters) {
  // stable = generate stable external ids and use these for ordering
  // counters = print stats about collisions
  Time x(times, "write");
  (void)graph;
  (void)root;
  (void)output;
  (void)stable;
  (void)counters;
}

}  // namespace
}  // namespace stg

int main(int argc, char* argv[]) {
  // Process arguments.
  bool opt_info = false;
  bool opt_counters = false;
  bool opt_times = false;
  bool opt_unstable = false;
  stg::InputFormat opt_input_format = stg::InputFormat::ABI;
  std::vector<const char*> inputs;
  std::vector<const char*> outputs;
  static option opts[] = {
      {"info",            no_argument,       nullptr, 'i'},
      {"counters",        no_argument,       nullptr, 'c'},
      {"times",           no_argument,       nullptr, 't'},
      {"unstable",        no_argument,       nullptr, 'u'},
      {"abi",             no_argument,       nullptr, 'a'},
      {"btf",             no_argument,       nullptr, 'b'},
      {"elf",             no_argument,       nullptr, 'e'},
      {"output",          required_argument, nullptr, 'o'},
      {nullptr,           0,                 nullptr, 0  },
  };
  auto usage = [&]() {
    std::cerr << "usage: " << argv[0] << '\n'
              << " [-i|--info]\n"
              << " [-c|--counters]\n"
              << " [-t|--times]\n"
              << " [-u|--unstable]\n"
              << " [-a|--abi|-b|--btf|-e|--elf] [file] ...\n"
              << " [{-o|--output} {filename|-}] ...\n"
              << "   implicit defaults: --abi\n"
              << "\n";
    return 1;
  };
  while (true) {
    int ix;
    int c = getopt_long(argc, argv, "-ictuabeo:", opts, &ix);
    if (c == -1)
      break;
    const char* argument = optarg;
    switch (c) {
      case 'i':
        opt_info = true;
        break;
      case 'c':
        opt_counters = true;
        break;
      case 't':
        opt_times = true;
        break;
      case 'u':
        opt_unstable = true;
        break;
      case 'a':
        opt_input_format = stg::InputFormat::ABI;
        break;
      case 'b':
        opt_input_format = stg::InputFormat::BTF;
        break;
      case 'e':
        opt_input_format = stg::InputFormat::ELF;
        break;
      case 1:
        inputs.push_back(argument);
        break;
      case 'o':
        if (strcmp(argument, "-") == 0)
          argument = "/dev/stdout";
        outputs.push_back(argument);
        break;
      default:
        return usage();
    }
  }

  try {
    stg::Graph graph;
    std::vector<stg::Id> roots;
    roots.reserve(inputs.size());
    for (auto input : inputs) {
      roots.push_back(stg::Read(graph, opt_input_format, input, opt_info));
    }
    stg::Id root = stg::Merge(graph, roots);
    for (auto output : outputs) {
      stg::Write(graph, root, output, !opt_unstable, opt_counters);
    }
    if (opt_times) {
      stg::Time::report(stg::times, std::cerr);
    }
    return 0;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
