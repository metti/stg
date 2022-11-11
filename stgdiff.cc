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
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
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
#include "error.h"
#include "reporting.h"
#include "stg.h"

namespace {

const int kAbiChange = 4;
const size_t kMaxCrcOnlyChanges = 3;
const stg::CompareOptions kAllCompareOptionsEnabled{true, true};

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

enum class InputFormat { ABI, BTF, ELF };

using Inputs = std::vector<std::pair<InputFormat, const char*>>;
using Outputs =
    std::vector<std::pair<stg::reporting::OutputFormat, const char*>>;

bool Run(const Inputs& inputs, const Outputs& outputs,
         const stg::CompareOptions& compare_options) {
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
      case InputFormat::ELF: {
        Time read("read ELF");
        roots.push_back(stg::elf::Read(graph, filename));
        break;
      }
    }
  }

  // Compute differences.
  stg::State state{graph, compare_options};
  std::pair<bool, std::optional<stg::Comparison>> result;
  {
    Time compute("compute diffs");
    result = stg::Compare(state, roots[0], roots[1]);
  }
  stg::Check(state.scc.Empty()) << "internal error: SCC state broken";
  const auto& [equals, comparison] = result;

  // Write reports.
  stg::NameCache names;
  for (const auto& [format, filename] : outputs) {
    std::ofstream output(filename);
    if (comparison) {
      Time report("report diffs");
      stg::reporting::Options options{format, kMaxCrcOnlyChanges};
      stg::reporting::Reporting reporting{graph, state.outcomes, options,
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
              << " [{-c|--compare-options} "
                 "{ignore_symbol_type_presence_changes|"
                 "ignore_type_declaration_status_changes|all}]\n"
              << " [{-f|--format} {plain|flat|small|short|viz}]\n"
              << " [{-o|--output} {filename|-}] ...\n"
              << "   implicit defaults: --abi --format plain\n"
              << "   format and output can appear multiple times\n"
              << "   multiple comma separated compare-options can be passed\n"
              << "\n";
    return 1;
  };
  while (true) {
    int ix;
    int c = getopt_long(argc, argv, "-tabec:f:o:", opts, &ix);
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
  if (inputs.size() != 2)
    return usage();

  try {
    const bool equals = Run(inputs, outputs, compare_options);
    if (opt_times)
      Time::report();
    return equals ? 0 : kAbiChange;
  } catch (const stg::Exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
