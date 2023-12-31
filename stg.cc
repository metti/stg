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
#include <utility>
#include <vector>

#include "deduplication.h"
#include "error.h"
#include "filter.h"
#include "fingerprint.h"
#include "graph.h"
#include "input.h"
#include "metrics.h"
#include "proto_writer.h"
#include "reader_options.h"
#include "type_resolution.h"
#include "unification.h"

namespace stg {
namespace {

struct GetInterface {
  Interface& operator()(Interface& x) const {
    return x;
  }

  template <typename Node>
  Interface& operator()(Node&) const {
    Die() << "expected an Interface root node";
  }
};

Id Merge(Graph& graph, const std::vector<Id>& roots, Metrics& metrics) {
  bool failed = false;
  // this rewrites the graph on destruction
  Unification unification(graph, Id(0), metrics);
  unification.Reserve(graph.Limit());
  std::map<std::string, Id> symbols;
  std::map<std::string, Id> types;
  const GetInterface get;
  for (auto root : roots) {
    const auto& interface = graph.Apply<Interface&>(get, root);
    for (const auto& x : interface.symbols) {
      if (!symbols.insert(x).second) {
        Warn() << "duplicate symbol during merge: " << x.first;
        failed = true;
      }
    }
    // TODO: test type roots merge
    for (const auto& x : interface.types) {
      const auto [it, inserted] = types.insert(x);
      if (!inserted && !unification.Unify(x.second, it->second)) {
        Warn() << "type conflict during merge: " << x.first;
        failed = true;
      }
    }
    graph.Remove(root);
  }
  if (failed) {
    Die() << "merge failed";
  }
  return graph.Add<Interface>(symbols, types);
}

void FilterSymbols(Graph& graph, Id root, const Filter& filter) {
  std::map<std::string, Id> symbols;
  GetInterface get;
  auto& interface = graph.Apply<Interface&>(get, root);
  for (const auto& x : interface.symbols) {
    if (filter(x.first)) {
      symbols.insert(x);
    }
  }
  std::swap(interface.symbols, symbols);
}

void Write(const Graph& graph, Id root, const char* output, Metrics& metrics) {
  std::ofstream os(output);
  {
    Time x(metrics, "write");
    proto::Writer writer(graph);
    writer.Write(root, os);
    os << std::flush;
  }
  if (!os) {
    Die() << "error writing to " << '\'' << output << '\'';
  }
}

}  // namespace
}  // namespace stg

int main(int argc, char* argv[]) {
  enum LongOptions {
    kSkipDwarf = 256,
  };
  // Process arguments.
  bool opt_metrics = false;
  bool opt_keep_duplicates = false;
  std::unique_ptr<stg::Filter> opt_file_filter;
  std::unique_ptr<stg::Filter> opt_symbol_filter;
  stg::ReadOptions opt_read_options;
  stg::InputFormat opt_input_format = stg::InputFormat::ABI;
  std::vector<const char*> inputs;
  std::vector<const char*> outputs;
  static option opts[] = {
      {"metrics",         no_argument,       nullptr, 'm'       },
      {"info",            no_argument,       nullptr, 'i'       },
      {"keep-duplicates", no_argument,       nullptr, 'd'       },
      {"types",           no_argument,       nullptr, 't'       },
      {"files",           required_argument, nullptr, 'F'       },
      {"file-filter",     required_argument, nullptr, 'F'       },
      {"symbols",         required_argument, nullptr, 'S'       },
      {"symbol-filter",   required_argument, nullptr, 'S'       },
      {"abi",             no_argument,       nullptr, 'a'       },
      {"btf",             no_argument,       nullptr, 'b'       },
      {"elf",             no_argument,       nullptr, 'e'       },
      {"stg",             no_argument,       nullptr, 's'       },
      {"output",          required_argument, nullptr, 'o'       },
      {"skip-dwarf",      no_argument,       nullptr, kSkipDwarf},
      {nullptr,           0,                 nullptr, 0         },
  };
  auto usage = [&]() {
    std::cerr << "usage: " << argv[0] << '\n'
              << "  [-m|--metrics]\n"
              << "  [-i|--info]\n"
              << "  [-d|--keep-duplicates]\n"
              << "  [-t|--types]\n"
              << "  [-F|--files|--file-filter <filter>]\n"
              << "  [-S|--symbols|--symbol-filter <filter>]\n"
              << "  [--skip-dwarf]\n"
              << "  [-a|--abi|-b|--btf|-e|--elf|-s|--stg] [file] ...\n"
              << "  [{-o|--output} {filename|-}] ...\n"
              << "implicit defaults: --abi\n";
    stg::FilterUsage(std::cerr);
    return 1;
  };
  while (true) {
    int ix;
    const int c = getopt_long(argc, argv, "-midtS:F:abeso:", opts, &ix);
    if (c == -1) {
      break;
    }
    const char* argument = optarg;
    switch (c) {
      case 'm':
        opt_metrics = true;
        break;
      case 'i':
        opt_read_options.Set(stg::ReadOptions::INFO);
        break;
      case 'd':
        opt_keep_duplicates = true;
        break;
      case 't':
        opt_read_options.Set(stg::ReadOptions::TYPE_ROOTS);
        break;
      case 'F':
        opt_file_filter = stg::MakeFilter(argument);
        break;
      case 'S':
        opt_symbol_filter = stg::MakeFilter(argument);
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
        if (strcmp(argument, "-") == 0) {
          argument = "/dev/stdout";
        }
        outputs.push_back(argument);
        break;
      case kSkipDwarf:
        opt_read_options.Set(stg::ReadOptions::SKIP_DWARF);
        break;
      default:
        return usage();
    }
  }

  try {
    stg::Graph graph;
    stg::Metrics metrics;
    std::vector<stg::Id> roots;
    roots.reserve(inputs.size());
    for (auto input : inputs) {
      roots.push_back(stg::Read(graph, opt_input_format, input,
                                opt_read_options, opt_file_filter,
                                metrics));
    }
    stg::Id root =
        roots.size() == 1 ? roots[0] : stg::Merge(graph, roots, metrics);
    if (opt_symbol_filter) {
      stg::FilterSymbols(graph, root, *opt_symbol_filter);
    }
    if (!opt_keep_duplicates) {
      {
        stg::Unification unification(graph, stg::Id(0), metrics);
        unification.Reserve(graph.Limit());
        stg::ResolveTypes(graph, unification, {root}, metrics);
        unification.Update(root);
      }
      const auto hashes = stg::Fingerprint(graph, root, metrics);
      root = stg::Deduplicate(graph, root, hashes, metrics);
    }
    for (auto output : outputs) {
      stg::Write(graph, root, output, metrics);
    }
    if (opt_metrics) {
      stg::Report(metrics, std::cerr);
    }
    return 0;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
