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
// Author: Siddharth Nayyar

#include <getopt.h>

#include <cstddef>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "abigail_reader.h"
#include "btf_reader.h"
#include "elf_reader.h"
#include "equality.h"
#include "error.h"
#include "graph.h"
#include "metrics.h"
#include "proto_reader.h"
#include "reporting.h"

namespace {

stg::Metrics metrics;

const int kAbiChange = 4;
const size_t kMaxCrcOnlyChanges = 3;
const stg::CompareOptions kAllCompareOptionsEnabled{true, true};

enum class InputFormat { ABI, BTF, ELF, STG };

using Inputs = std::vector<std::pair<InputFormat, const char*>>;
using Outputs =
    std::vector<std::pair<stg::reporting::OutputFormat, const char*>>;

std::vector<stg::Id> Read(const Inputs& inputs, stg::Graph& graph,
                          bool process_dwarf, stg::Metrics& metrics) {
  std::vector<stg::Id> roots;
  for (const auto& [format, filename] : inputs) {
    switch (format) {
      case InputFormat::ABI: {
        stg::Time read(metrics, "read ABI");
        roots.push_back(stg::abixml::Read(graph, filename, metrics));
        break;
      }
      case InputFormat::BTF: {
        stg::Time read(metrics, "read BTF");
        roots.push_back(stg::btf::ReadFile(graph, filename));
        break;
      }
      case InputFormat::ELF: {
        stg::Time read(metrics, "read ELF");
        roots.push_back(stg::elf::Read(graph, filename, process_dwarf,
                                       /* verbose = */ false));
        break;
      }
      case InputFormat::STG: {
        stg::Time read(metrics, "read STG");
        roots.push_back(stg::proto::Read(graph, filename));
        break;
      }
    }
  }
  return roots;
}

bool RunExact(const Inputs& inputs, bool process_dwarf, stg::Metrics& metrics) {
  stg::Graph graph;
  const auto roots = Read(inputs, graph, process_dwarf, metrics);

  struct PairCache {
    std::optional<bool> Query(const stg::Pair& comparison) const {
      return equalities.find(comparison) != equalities.end()
          ? std::make_optional(true)
          : std::nullopt;
    }
    void AllSame(const std::vector<stg::Pair>& comparisons) {
      for (const auto& comparison : comparisons) {
        equalities.insert(comparison);
      }
    }
    void AllDifferent(const std::vector<stg::Pair>&) {}
    std::unordered_set<stg::Pair, stg::HashPair> equalities;
  };

  stg::Time compute(metrics, "equality check");
  PairCache equalities;
  return stg::Equals<PairCache>(graph, equalities)(roots[0], roots[1]);
}

bool Run(const Inputs& inputs, const Outputs& outputs,
         const stg::CompareOptions& compare_options, bool process_dwarf,
         stg::Metrics& metrics) {
  // Read inputs.
  stg::Graph graph;
  const auto roots = Read(inputs, graph, process_dwarf, metrics);

  // Compute differences.
  stg::Compare compare{graph, compare_options, metrics};
  std::pair<bool, std::optional<stg::Comparison>> result;
  {
    stg::Time compute(metrics, "compute diffs");
    result = compare(roots[0], roots[1]);
  }
  stg::Check(compare.scc.Empty()) << "internal error: SCC state broken";
  const auto& [equals, comparison] = result;

  // Write reports.
  stg::NameCache names;
  for (const auto& [format, filename] : outputs) {
    std::ofstream output(filename);
    if (comparison) {
      stg::Time report(metrics, "report diffs");
      stg::reporting::Options options{format, kMaxCrcOnlyChanges};
      stg::reporting::Reporting reporting{graph, compare.outcomes, options,
        names};
      Report(reporting, *comparison, output);
      output << std::flush;
    }
    if (!output)
      stg::Die() << "error writing to " << '\'' << filename << '\'';
  }

  return equals;
}

}  // namespace

bool ParseCompareOptions(const char* opts_arg, stg::CompareOptions& opts) {
  std::stringstream opt_stream(opts_arg);
  std::string opt;
  while (std::getline(opt_stream, opt, ',')) {
    if (opt == "ignore_symbol_type_presence_changes")
      opts.ignore_symbol_type_presence_changes = true;
    else if (opt == "ignore_type_declaration_status_changes")
      opts.ignore_type_declaration_status_changes = true;
    else if (opt == "all")
      opts = kAllCompareOptionsEnabled;
    else
      return false;
  }
  return true;
}

enum LongOptions {
  kProcessDwarf = 256,
};

int main(int argc, char* argv[]) {
  // Process arguments.
  bool opt_metrics = false;
  bool opt_exact = false;
  bool opt_process_dwarf = false;
  stg::CompareOptions compare_options;
  InputFormat opt_input_format = InputFormat::ABI;
  stg::reporting::OutputFormat opt_output_format =
      stg::reporting::OutputFormat::PLAIN;
  Inputs inputs;
  Outputs outputs;
  static option opts[] = {
      {"metrics",         no_argument,       nullptr, 'm'          },
      {"abi",             no_argument,       nullptr, 'a'          },
      {"btf",             no_argument,       nullptr, 'b'          },
      {"elf",             no_argument,       nullptr, 'e'          },
      {"stg",             no_argument,       nullptr, 's'          },
      {"exact",           no_argument,       nullptr, 'x'          },
      {"compare-options", required_argument, nullptr, 'c'          },
      {"format",          required_argument, nullptr, 'f'          },
      {"output",          required_argument, nullptr, 'o'          },
      {"process-dwarf",   no_argument,       nullptr, kProcessDwarf},
      {nullptr,           0,                 nullptr, 0  },
  };
  auto usage = [&]() {
    std::cerr << "usage: " << argv[0] << '\n'
              << " [-m|--metrics]\n"
              << " [-a|--abi|-b|--btf|-e|--elf|-s|--stg] file1\n"
              << " [-a|--abi|-b|--btf|-e|--elf|-s|--stg] file2\n"
              << " [{-x|--exact}]\n"
              << " [--process-dwarf]\n"
              << " [{-c|--compare-options} "
                 "{ignore_symbol_type_presence_changes|"
                 "ignore_type_declaration_status_changes|all}]\n"
              << " [{-f|--format} {plain|flat|small|short|viz}]\n"
              << " [{-o|--output} {filename|-}] ...\n"
              << "   implicit defaults: --abi --format plain\n"
              << "   format and output can appear multiple times\n"
              << "   multiple comma-separated compare-options can be passed\n"
              << "   --exact (node equality) cannot be combined with --output\n"
              << "\n";
    return 1;
  };
  while (true) {
    int ix;
    int c = getopt_long(argc, argv, "-mabesxc:f:o:", opts, &ix);
    if (c == -1)
      break;
    const char* argument = optarg;
    switch (c) {
      case 'm':
        opt_metrics = true;
        break;
      case 'a':
        opt_input_format = InputFormat::ABI;
        break;
      case 'b':
        opt_input_format = InputFormat::BTF;
        break;
      case 'e':
        opt_input_format = InputFormat::ELF;
        break;
      case 's':
        opt_input_format = InputFormat::STG;
        break;
      case 'x':
        opt_exact = true;
        break;
      case 1:
        inputs.push_back({opt_input_format, argument});
        break;
      case 'c':
        if (!ParseCompareOptions(argument, compare_options))
          return usage();
        break;
      case 'f':
        if (strcmp(argument, "plain") == 0)
          opt_output_format = stg::reporting::OutputFormat::PLAIN;
        else if (strcmp(argument, "flat") == 0)
          opt_output_format = stg::reporting::OutputFormat::FLAT;
        else if (strcmp(argument, "small") == 0)
          opt_output_format = stg::reporting::OutputFormat::SMALL;
        else if (strcmp(argument, "short") == 0)
          opt_output_format = stg::reporting::OutputFormat::SHORT;
        else if (strcmp(argument, "viz") == 0)
          opt_output_format = stg::reporting::OutputFormat::VIZ;
        else
          return usage();
        break;
      case 'o':
        if (strcmp(argument, "-") == 0)
          argument = "/dev/stdout";
        outputs.push_back({opt_output_format, argument});
        break;
      case kProcessDwarf:
        opt_process_dwarf = true;
        break;
      default:
        return usage();
    }
  }
  if (inputs.size() != 2 || opt_exact > outputs.empty()) {
    return usage();
  }

  try {
    const bool equals = opt_exact
        ? RunExact(inputs, opt_process_dwarf, metrics)
        : Run(inputs, outputs, compare_options, opt_process_dwarf, metrics);
    if (opt_metrics) {
      stg::Report(metrics, std::cerr);
    }
    return equals ? 0 : kAbiChange;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
