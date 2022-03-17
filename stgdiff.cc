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

#include <getopt.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "abigail_reader.h"
#include "btf_reader.h"
#include "error.h"

namespace {

const int kAbiChange = 4;

class Time {
 public:
  Time(const char* what) : what_(what) {
    clock_gettime(CLOCK_MONOTONIC, &start_);
  }

  ~Time() {
    struct timespec finish;
    clock_gettime(CLOCK_MONOTONIC, &finish);
    auto seconds = finish.tv_sec - start_.tv_sec;
    auto nanos = finish.tv_nsec - start_.tv_nsec;
    times_.emplace_back(what_, seconds * 1'000'000'000 + nanos);
  }

  static void report() {
    for (const auto& [what, ns] : times_) {
      auto millis = ns / 1'000'000;
      auto nanos = ns % 1'000'000;
      std::cerr << what << ": " << millis << '.' << std::setfill('0')
                << std::setw(6) << nanos << std::setfill(' ') << " ms\n";
    }
  }

 private:
  const char* what_;
  struct timespec start_;
  static std::vector<std::pair<const char*, uint64_t>> times_;
};

std::vector<std::pair<const char*, uint64_t>> Time::times_;

enum class InputFormat { ABI, BTF };
enum class OutputFormat { PLAIN, FLAT, SMALL, VIZ };

using Inputs = std::vector<std::pair<InputFormat, const char*>>;
using Outputs = std::vector<std::pair<OutputFormat, const char*>>;

void ReportPlain(const stg::Comparison& comparison,
                 const stg::Outcomes& outcomes,
                 stg::NameCache& names, std::ostream& output) {
  // unpack then print - want symbol diff forest rather than symbols diff tree
  const auto& diff = outcomes.at(comparison);
  stg::Seen seen;
  stg::Print(diff.details, outcomes, seen, names, output);
}

void ReportFlat(bool full, const stg::Comparison& comparison,
                const stg::Outcomes& outcomes, stg::NameCache& names,
                std::ostream& output) {
  // We want a symbol diff forest rather than a symbol table diff tree, so
  // unpack the symbol table and then print the symbols specially.
  const auto& diff = outcomes.at(comparison);
  std::unordered_set<stg::Comparison, stg::HashComparison> seen;
  std::deque<stg::Comparison> todo;
  for (const auto& detail : diff.details) {
    std::ostringstream os;
    const bool interesting = stg::FlatPrint(
        *detail.edge_, outcomes, seen, todo, full, true, names, os);
    if (interesting || full)
      output << os.str() << '\n';
  }
  while (!todo.empty()) {
    auto comp = todo.front();
    todo.pop_front();
    std::ostringstream os;
    const bool interesting =
        stg::FlatPrint(comp, outcomes, seen, todo, full, false, names, os);
    if (interesting || full)
      output << os.str() << '\n';
  }
}

void ReportViz(const stg::Comparison& comparison, const stg::Outcomes& outcomes,
               stg::NameCache& names, std::ostream& output) {
  output << "digraph \"ABI diff\" {\n";
  std::unordered_set<stg::Comparison, stg::HashComparison> seen;
  std::unordered_map<stg::Comparison, size_t, stg::HashComparison> ids;
  stg::VizPrint(comparison, outcomes, seen, ids, names, output);
  output << "}\n";
}

bool Report(const Inputs& inputs, const Outputs& outputs) {
  // Read inputs.
  std::vector<std::unique_ptr<stg::Graph>> graphs;
  for (const auto& [format, filename] : inputs) {
    graphs.push_back({});
    auto& graph = graphs.back();
    switch (format) {
      case InputFormat::ABI: {
        Time read("read ABI");
        graph = stg::abixml::Read(filename);
        break;
      }
      case InputFormat::BTF: {
        Time read("read BTF");
        graph = stg::btf::ReadFile(filename);
        break;
      }
    }
  }

  // Compute differences.
  stg::State state;
  std::pair<bool, std::optional<stg::Comparison>> result;
  {
    Time compute("compute diffs");
    const stg::Type& lhs = graphs[0]->GetRoot();
    const stg::Type& rhs = graphs[1]->GetRoot();
    result = stg::Compare(state, lhs, rhs);
  }
  stg::Check(state.scc.Empty()) << "internal error: SCC state broken";
  const auto& [equals, comparison] = result;

  // Write reports.
  stg::NameCache names;
  const auto& outcomes = state.outcomes;
  for (const auto& [format, filename] : outputs) {
    std::ofstream output(filename);
    if (comparison) {
      Time report("report diffs");
      switch (format) {
        case OutputFormat::PLAIN: {
          ReportPlain(*comparison, outcomes, names, output);
          break;
        }
        case OutputFormat::FLAT:
        case OutputFormat::SMALL: {
          bool full = format == OutputFormat::FLAT;
          ReportFlat(full, *comparison, outcomes, names, output);
          break;
        }
        case OutputFormat::VIZ: {
          ReportViz(*comparison, outcomes, names, output);
          break;
        }
      }
      output << std::flush;
    }
    if (!output)
      stg::Die() << "error writing to " << '\'' << filename << '\'';
  }
  return equals;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Process arguments.
  bool opt_times = false;
  InputFormat opt_input_format = InputFormat::ABI;
  OutputFormat opt_output_format = OutputFormat::PLAIN;
  Inputs inputs;
  Outputs outputs;
  static option opts[] = {
      {"times",  no_argument,       nullptr, 't'},
      {"abi",    no_argument,       nullptr, 'a'},
      {"btf",    no_argument,       nullptr, 'b'},
      {"format", required_argument, nullptr, 'f'},
      {"output", required_argument, nullptr, 'o'},
      {nullptr,  0,                 nullptr, 0  },
  };
  auto usage = [&]() {
    std::cerr << "usage: " << argv[0] << '\n'
              << " [-t|--times]\n"
              << " [-a|--abi|-b|--btf] file1\n"
              << " [-a|--abi|-b|--btf] file2\n"
              << " [{-f|--format} {plain|flat|small|viz}]\n"
              << " [{-o|--output} {filename|-}] ...\n"
              << "   implicit defaults: --abi --format plain\n"
              << "   format and output can appear multiple times\n"
              << "\n";
    return 1;
  };
  while (true) {
    int ix;
    int c = getopt_long(argc, argv, "-tabf:o:", opts, &ix);
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
      case 1:
        inputs.push_back({opt_input_format, argument});
        break;
      case 'f':
        if (strcmp(argument, "plain") == 0)
          opt_output_format = OutputFormat::PLAIN;
        else if (strcmp(argument, "flat") == 0)
          opt_output_format = OutputFormat::FLAT;
        else if (strcmp(argument, "small") == 0)
          opt_output_format = OutputFormat::SMALL;
        else if (strcmp(argument, "viz") == 0)
          opt_output_format = OutputFormat::VIZ;
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
  if (inputs.size() != 2)
    return usage();

  try {
    const bool equals = Report(inputs, outputs);
    if (opt_times)
      Time::report();
    return equals ? 0 : kAbiChange;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
