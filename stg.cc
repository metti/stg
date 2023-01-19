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

#include <getopt.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "abigail_reader.h"
#include "btf_reader.h"
#include "deduplication.h"
#include "elf_reader.h"
#include "error.h"
#include "fingerprint.h"
#include "graph.h"
#include "metrics.h"
#include "proto_reader.h"
#include "proto_writer.h"
#include "symbol_filter.h"
#include "type_resolution.h"

namespace stg {
namespace {

Metrics metrics;

enum class InputFormat { ABI, BTF, ELF, STG };

Id Read(Graph& graph, InputFormat format, const char* input, bool process_dwarf,
        bool info, Metrics& metrics) {
  switch (format) {
    case InputFormat::ABI: {
      Time read(metrics, "read ABI");
      return abixml::Read(graph, input, metrics);
    }
    case InputFormat::BTF: {
      Time read(metrics, "read BTF");
      return btf::ReadFile(graph, input, info);
    }
    case InputFormat::ELF: {
      Time read(metrics, "read ELF");
      return elf::Read(graph, input, process_dwarf, info);
    }
    case InputFormat::STG: {
      Time read(metrics, "read STG");
      return proto::Read(graph, input);
    }
  }
}

struct GetSymbols {
  const Symbols& operator()(const Symbols& x) {
    return x;
  }

  template <typename Node>
  const Symbols& operator()(const Node&) {
    Die() << "expected a Symbols root node";
  }
};

Id Merge(Graph& graph, const std::vector<Id>& roots) {
  std::map<std::string, Id> symbols;
  GetSymbols get;
  for (auto root : roots) {
    for (const auto& x : graph.Apply<const Symbols&>(get, root).symbols) {
      if (!symbols.insert(x).second) {
        Die() << "merge failed with duplicate symbol: " << x.first;
      }
    }
    graph.Remove(root);
  }
  return graph.Add<Symbols>(symbols);
}

void Filter(Graph& graph, Id root, const SymbolFilter& filter) {
  std::map<std::string, Id> symbols;
  GetSymbols get;
  for (const auto& x : graph.Apply<const Symbols&>(get, root).symbols) {
    if (filter(x.first)) {
      symbols.insert(x);
    }
  }
  graph.Unset(root);
  graph.Set<Symbols>(root, symbols);
}

void Write(const Graph& graph, Id root, const char* output, bool stable) {
  std::ofstream os(output);
  {
    Time x(metrics, "write");
    proto::Writer writer(graph, stable);
    writer.Write(root, os);
    os << std::flush;
  }
  if (!os) {
    Die() << "error writing to " << '\'' << output << '\'';
  }
}

}  // namespace
}  // namespace stg

enum LongOptions {
  kProcessDwarf = 256,
};

int main(int argc, char* argv[]) {
  // Process arguments.
  bool opt_metrics = false;
  bool opt_info = false;
  bool opt_keep_duplicates = false;
  bool opt_unstable = false;
  std::unique_ptr<stg::SymbolFilter> opt_symbols;
  bool opt_process_dwarf = false;
  stg::InputFormat opt_input_format = stg::InputFormat::ABI;
  std::vector<const char*> inputs;
  std::vector<const char*> outputs;
  static option opts[] = {
      {"metrics",         no_argument,       nullptr, 'm'          },
      {"info",            no_argument,       nullptr, 'i'          },
      {"keep-duplicates", no_argument,       nullptr, 'd'          },
      {"unstable",        no_argument,       nullptr, 'u'          },
      {"symbols",         required_argument, nullptr, 'S'          },
      {"abi",             no_argument,       nullptr, 'a'          },
      {"btf",             no_argument,       nullptr, 'b'          },
      {"elf",             no_argument,       nullptr, 'e'          },
      {"stg",             no_argument,       nullptr, 's'          },
      {"output",          required_argument, nullptr, 'o'          },
      {"process-dwarf",   no_argument,       nullptr, kProcessDwarf},
      {nullptr,           0,                 nullptr, 0  },
  };
  auto usage = [&]() {
    std::cerr << "usage: " << argv[0] << '\n'
              << "  [-m|--metrics]\n"
              << "  [-i|--info]\n"
              << "  [-d|--keep-duplicates]\n"
              << "  [-u|--unstable]\n"
              << "  [-S|--symbols <filter>]\n"
              << "  [--process-dwarf]\n"
              << "  [-a|--abi|-b|--btf|-e|--elf|-s|--stg] [file] ...\n"
              << "  [{-o|--output} {filename|-}] ...\n"
              << "implicit defaults: --abi\n";
    stg::SymbolFilterUsage(std::cerr);
    return 1;
  };
  while (true) {
    int ix;
    int c = getopt_long(argc, argv, "-miduS:abeso:", opts, &ix);
    if (c == -1)
      break;
    const char* argument = optarg;
    switch (c) {
      case 'm':
        opt_metrics = true;
        break;
      case 'i':
        opt_info = true;
        break;
      case 'd':
        opt_keep_duplicates = true;
        break;
      case 'u':
        opt_unstable = true;
        break;
      case 'S':
        opt_symbols = stg::MakeSymbolFilter(argument);
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
      case 's':
        opt_input_format = stg::InputFormat::STG;
        break;
      case 1:
        inputs.push_back(argument);
        break;
      case 'o':
        if (strcmp(argument, "-") == 0)
          argument = "/dev/stdout";
        outputs.push_back(argument);
        break;
      case kProcessDwarf:
        opt_process_dwarf = true;
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
      roots.push_back(stg::Read(graph, opt_input_format, input,
                                opt_process_dwarf, opt_info, stg::metrics));
    }
    stg::Id root = roots.size() == 1 ? roots[0] : stg::Merge(graph, roots);
    if (opt_symbols) {
      stg::Filter(graph, root, *opt_symbols);
    }
    if (!opt_keep_duplicates) {
      root = stg::ResolveTypes(graph, root, stg::metrics);
      const auto hashes = stg::Fingerprint(graph, root, stg::metrics);
      root = stg::Deduplicate(graph, root, hashes, stg::metrics);
    }
    for (auto output : outputs) {
      stg::Write(graph, root, output, !opt_unstable);
    }
    if (opt_metrics) {
      stg::Report(stg::metrics, std::cerr);
    }
    return 0;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
