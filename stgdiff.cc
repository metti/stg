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
#include "reporting.h"
#include "timing.h"

namespace {

stg::Times times;

const int kAbiChange = 4;
const size_t kMaxCrcOnlyChanges = 3;
const stg::CompareOptions kAllCompareOptionsEnabled{true, true};

enum class InputFormat { ABI, BTF, ELF };

using Inputs = std::vector<std::pair<InputFormat, const char*>>;
using Outputs =
    std::vector<std::pair<stg::reporting::OutputFormat, const char*>>;

std::vector<stg::Id> Read(const Inputs& inputs, stg::Graph& graph) {
  std::vector<stg::Id> roots;
  for (const auto& [format, filename] : inputs) {
    switch (format) {
      case InputFormat::ABI: {
        stg::Time read(times, "read ABI");
        roots.push_back(stg::abixml::Read(graph, filename));
        break;
      }
      case InputFormat::BTF: {
        stg::Time read(times, "read BTF");
        roots.push_back(stg::btf::ReadFile(graph, filename));
        break;
      }
      case InputFormat::ELF: {
        stg::Time read(times, "read ELF");
        roots.push_back(stg::elf::Read(graph, filename));
        break;
      }
    }
  }
  return roots;
}

bool RunExact(const Inputs& inputs) {
  stg::Graph graph;
  const auto roots = Read(inputs, graph);

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

  stg::Time compute(times, "equality check");
  PairCache equalities;
  return stg::Equals<PairCache>(graph, equalities)(roots[0], roots[1]);
}

bool Run(const Inputs& inputs, const Outputs& outputs,
         const stg::CompareOptions& compare_options) {
  // Read inputs.
  stg::Graph graph;
  const auto roots = Read(inputs, graph);

  // Compute differences.
  stg::Compare compare{graph, compare_options};
  std::pair<bool, std::optional<stg::Comparison>> result;
  {
    stg::Time compute(times, "compute diffs");
    result = compare(roots[0], roots[1]);
  }
  stg::Check(compare.scc.Empty()) << "internal error: SCC state broken";
  const auto& [equals, comparison] = result;

  // Write reports.
  stg::NameCache names;
  for (const auto& [format, filename] : outputs) {
    std::ofstream output(filename);
    if (comparison) {
      stg::Time report(times, "report diffs");
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

int main(int argc, char* argv[]) {
  // Process arguments.
  bool opt_times = false;
  bool opt_exact = false;
  stg::CompareOptions compare_options;
  InputFormat opt_input_format = InputFormat::ABI;
  stg::reporting::OutputFormat opt_output_format =
      stg::reporting::OutputFormat::PLAIN;
  Inputs inputs;
  Outputs outputs;
  static option opts[] = {
      {"times",           no_argument,       nullptr, 't'},
      {"abi",             no_argument,       nullptr, 'a'},
      {"btf",             no_argument,       nullptr, 'b'},
      {"elf",             no_argument,       nullptr, 'e'},
      {"exact",           no_argument,       nullptr, 'x'},
      {"compare-options", required_argument, nullptr, 'c'},
      {"format",          required_argument, nullptr, 'f'},
      {"output",          required_argument, nullptr, 'o'},
      {nullptr,           0,                 nullptr, 0  },
  };
  auto usage = [&]() {
    std::cerr << "usage: " << argv[0] << '\n'
              << " [-t|--times]\n"
              << " [-a|--abi|-b|--btf|-e|--elf] file1\n"
              << " [-a|--abi|-b|--btf|-e|--elf] file2\n"
              << " [{-x|--exact}]\n"
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
    int c = getopt_long(argc, argv, "-tabexc:f:o:", opts, &ix);
    if (c == -1)
      break;
    const char* argument = optarg;
    switch (c) {
      case 't':
        opt_times = true;
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
      default:
        return usage();
    }
  }
  if (inputs.size() != 2 || opt_exact > outputs.empty()) {
    return usage();
  }

  try {
    const bool equals = opt_exact
                        ? RunExact(inputs)
                        : Run(inputs, outputs, compare_options);
    if (opt_times) {
      stg::Time::report(times, std::cerr);
    }
    return equals ? 0 : kAbiChange;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
