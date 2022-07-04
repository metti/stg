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
#include "reporting.h"

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

using Inputs = std::vector<std::pair<InputFormat, const char*>>;
using Outputs = std::vector<std::pair<stg::OutputFormat, const char*>>;

bool Run(const Inputs& inputs, const Outputs& outputs) {
  // Read inputs.
  stg::Graph graph;
  std::vector<stg::Id> roots;
  for (const auto& [format, filename] : inputs) {
    switch (format) {
      case InputFormat::ABI: {
        Time read("read ABI");
        roots.push_back(stg::abixml::Read(graph, filename));
        break;
      }
      case InputFormat::BTF: {
        Time read("read BTF");
        roots.push_back(stg::btf::ReadFile(graph, filename));
        break;
      }
    }
  }

  // Compute differences.
  stg::State state{graph};
  std::pair<bool, std::optional<stg::Comparison>> result;
  {
    Time compute("compute diffs");
    result = stg::Compare(state, roots[0], roots[1]);
  }
  stg::Check(state.scc.Empty()) << "internal error: SCC state broken";
  const auto& [equals, comparison] = result;

  // Write reports.
  stg::NameCache names;
  stg::Reporting reporting{graph, state.outcomes, names};
  for (const auto& [format, filename] : outputs) {
    std::ofstream output(filename);
    if (comparison) {
      Time report("report diffs");
      Report(reporting, *comparison, format, output);
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
  stg::OutputFormat opt_output_format = stg::OutputFormat::PLAIN;
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
          opt_output_format = stg::OutputFormat::PLAIN;
        else if (strcmp(argument, "flat") == 0)
          opt_output_format = stg::OutputFormat::FLAT;
        else if (strcmp(argument, "small") == 0)
          opt_output_format = stg::OutputFormat::SMALL;
        else if (strcmp(argument, "viz") == 0)
          opt_output_format = stg::OutputFormat::VIZ;
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
    const bool equals = Run(inputs, outputs);
    if (opt_times)
      Time::report();
    return equals ? 0 : kAbiChange;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
