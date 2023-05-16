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
// Author: Siddharth Nayyar

#include <getopt.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "equality.h"
#include "error.h"
#include "fidelity.h"
#include "graph.h"
#include "input.h"
#include "metrics.h"
#include "reader_options.h"
#include "reporting.h"

namespace {

stg::Metrics metrics;

const int kAbiChange = 4;
const int kFidelityChange = 8;
const size_t kMaxCrcOnlyChanges = 3;

using Inputs = std::vector<std::pair<stg::InputFormat, const char*>>;
using Outputs =
    std::vector<std::pair<stg::reporting::OutputFormat, const char*>>;

std::vector<stg::Id> Read(const Inputs& inputs, stg::Graph& graph,
                          stg::ReadOptions options, stg::Metrics& metrics) {
  std::vector<stg::Id> roots;
  for (const auto& [format, filename] : inputs) {
    roots.push_back(stg::Read(graph, format, filename, options, metrics));
  }
  return roots;
}

int RunFidelity(const char* filename, const stg::Graph& graph,
                const std::vector<stg::Id>& roots) {
  std::ofstream output(filename);
  auto fidelity_diff = stg::GetFidelityTransitions(graph, roots[0], roots[1]);
  stg::reporting::FidelityDiff(fidelity_diff, output);
  output << std::flush;
  if (!output) {
    stg::Die() << "error writing to " << '\'' << filename << '\'';
  }
  return fidelity_diff.severity == stg::FidelityDiffSeverity::WARN
             ? kFidelityChange
             : 0;
}

int RunExact(const Inputs& inputs, stg::ReadOptions options,
             stg::Metrics& metrics) {
  stg::Graph graph;
  const auto roots = Read(inputs, graph, options, metrics);

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
    std::unordered_set<stg::Pair> equalities;
  };

  stg::Time compute(metrics, "equality check");
  PairCache equalities;
  return stg::Equals<PairCache>(graph, equalities)(roots[0], roots[1])
             ? 0
             : kAbiChange;
}

int Run(const Inputs& inputs, const Outputs& outputs, stg::Ignore ignore,
        stg::ReadOptions options, std::optional<const char*> fidelity,
        stg::Metrics& metrics) {
  // Read inputs.
  stg::Graph graph;
  const auto roots = Read(inputs, graph, options, metrics);

  // Compute differences.
  stg::Compare compare{graph, ignore, metrics};
  std::pair<bool, std::optional<stg::Comparison>> result;
  {
    stg::Time compute(metrics, "compute diffs");
    result = compare(roots[0], roots[1]);
  }
  stg::Check(compare.scc.Empty()) << "internal error: SCC state broken";
  const auto& [equals, comparison] = result;
  int status = equals ? 0 : kAbiChange;

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
    if (!output) {
      stg::Die() << "error writing to " << '\'' << filename << '\'';
    }
  }

  // Compute fidelity diff if requested.
  if (fidelity) {
    const stg::Time report(metrics, "fidelity");
    status |= RunFidelity(*fidelity, graph, roots);
  }

  return status;
}

}  // namespace

int main(int argc, char* argv[]) {
  enum LongOptions {
    kSkipDwarf = 256,
  };
  // Process arguments.
  bool opt_metrics = false;
  bool opt_exact = false;
  stg::ReadOptions opt_read_options;
  std::optional<const char*> opt_fidelity = std::nullopt;
  stg::Ignore opt_ignore;
  stg::InputFormat opt_input_format = stg::InputFormat::ABI;
  stg::reporting::OutputFormat opt_output_format =
      stg::reporting::OutputFormat::PLAIN;
  Inputs inputs;
  Outputs outputs;
  static option opts[] = {
      {"metrics",        no_argument,       nullptr, 'm'       },
      {"abi",            no_argument,       nullptr, 'a'       },
      {"btf",            no_argument,       nullptr, 'b'       },
      {"elf",            no_argument,       nullptr, 'e'       },
      {"stg",            no_argument,       nullptr, 's'       },
      {"exact",          no_argument,       nullptr, 'x'       },
      {"types",          no_argument,       nullptr, 't'       },
      {"ignore",         required_argument, nullptr, 'i'       },
      {"format",         required_argument, nullptr, 'f'       },
      {"output",         required_argument, nullptr, 'o'       },
      {"fidelity",       required_argument, nullptr, 'F'       },
      {"skip-dwarf",     no_argument,       nullptr, kSkipDwarf},
      {nullptr,          0,                 nullptr, 0         },
  };
  auto usage = [&]() {
    std::cerr << "usage: " << argv[0] << '\n'
              << "  [-m|--metrics]\n"
              << "  [-a|--abi|-b|--btf|-e|--elf|-s|--stg] file1\n"
              << "  [-a|--abi|-b|--btf|-e|--elf|-s|--stg] file2\n"
              << "  [-x|--exact]\n"
              << "  [-t|--types]\n"
              << "  [--skip-dwarf]\n"
              << "  [{-i|--ignore} <ignore-option>] ...\n"
              << "  [{-f|--format} <output-format>] ...\n"
              << "  [{-o|--output} {filename|-}] ...\n"
              << "  [{-F|--fidelity} {filename|-}]\n"
              << "implicit defaults: --abi --format plain\n"
              << "--exact (node equality) cannot be combined with --output\n"
              << stg::reporting::OutputFormatUsage()
              << stg::IgnoreUsage();
    return 1;
  };
  while (true) {
    int ix;
    const int c = getopt_long(argc, argv, "-mabesxti:f:o:F:", opts, &ix);
    if (c == -1) {
      break;
    }
    const char* argument = optarg;
    switch (c) {
      case 'm':
        opt_metrics = true;
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
      case 'x':
        opt_exact = true;
        break;
      case 't':
        opt_read_options.Set(stg::ReadOptions::TYPE_ROOTS);
        break;
      case 1:
        inputs.emplace_back(opt_input_format, argument);
        break;
      case 'i':
        if (const auto ignore = stg::ParseIgnore(argument)) {
          opt_ignore.Set(ignore.value());
        } else {
          std::cerr << "unknown ignore option: " << argument << '\n'
                    << stg::IgnoreUsage();
          return 1;
        }
        break;
      case 'f':
        if (const auto format = stg::reporting::ParseOutputFormat(argument)) {
          opt_output_format = format.value();
        } else {
          std::cerr << "unknown output format: " << argument << '\n'
                    << stg::reporting::OutputFormatUsage();
          return 1;
        }
        break;
      case 'o':
        if (strcmp(argument, "-") == 0) {
          argument = "/dev/stdout";
        }
        outputs.emplace_back(opt_output_format, argument);
        break;
      case 'F':
        if (strcmp(argument, "-") == 0) {
          argument = "/dev/stdout";
        }
        opt_fidelity.emplace(argument);
        break;
      case kSkipDwarf:
        opt_read_options.Set(stg::ReadOptions::SKIP_DWARF);
        break;
      default:
        return usage();
    }
  }
  if (inputs.size() != 2 || opt_exact > outputs.empty()) {
    return usage();
  }

  try {
    const int status = opt_exact ? RunExact(inputs, opt_read_options, metrics)
                                 : Run(inputs, outputs, opt_ignore,
                                       opt_read_options, opt_fidelity, metrics);
    if (opt_metrics) {
      stg::Report(metrics, std::cerr);
    }
    return status;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
