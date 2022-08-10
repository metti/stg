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
// Author: Siddharth Nayyar

#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

#include <catch2/catch.hpp>
#include "abigail_reader.h"
#include "reporting.h"
#include "stg.h"

struct CompareOptionsTestCase {
  const std::string name;
  const std::string xml0;
  const std::string xml1;
  const stg::CompareOptions compare_options;
  const std::string expected_output;
  const bool expected_equals;
};

std::string filename_to_path(const std::string& f) {
  return std::filesystem::path("testdata") / f;
}

TEST_CASE("compare options") {
  const auto test_case =
      GENERATE(CompareOptionsTestCase({"symbol type presence change",
                                       "symbol_type_presence_0.xml",
                                       "symbol_type_presence_1.xml",
                                       {false, false},
                                       "symbol_type_presence_small_diff",
                                       false}),
               CompareOptionsTestCase({"symbol type presence change pruned",
                                       "symbol_type_presence_0.xml",
                                       "symbol_type_presence_1.xml",
                                       {true, false},
                                       "empty",
                                       true}),
               CompareOptionsTestCase({"type declaration status change",
                                       "type_declaration_status_0.xml",
                                       "type_declaration_status_1.xml",
                                       {false, false},
                                       "type_declaration_status_small_diff",
                                       false}),
               CompareOptionsTestCase({"type declaration status change pruned",
                                       "type_declaration_status_0.xml",
                                       "type_declaration_status_1.xml",
                                       {false, true},
                                       "empty",
                                       true}));

  SECTION(test_case.name) {
    // Read inputs.
    stg::Graph graph;
    const auto id0 = stg::abixml::Read(graph, filename_to_path(test_case.xml0));
    const auto id1 = stg::abixml::Read(graph, filename_to_path(test_case.xml1));

    // Compute differences.
    stg::State state{graph, test_case.compare_options};
    const auto& [equals, comparison] = stg::Compare(state, id0, id1);

    // Write SMALL reports.
    std::ostringstream output;
    if (comparison) {
      stg::NameCache names;
      stg::ReportingOptions options{stg::OutputFormat::SMALL, 0};
      stg::Reporting reporting{graph, state.outcomes, options, names};
      Report(reporting, *comparison, output);
    }

    // Check comparison outcome and report output.
    CHECK(equals == test_case.expected_equals);
    std::ifstream expected_output_file(
        filename_to_path(test_case.expected_output));
    std::ostringstream expected_output;
    expected_output << expected_output_file.rdbuf();
    CHECK(output.str() == expected_output.str());
  }
}

struct ShortReportTestCase {
  const std::string name;
  const std::string xml0;
  const std::string xml1;
  const std::string expected_output;
};

TEST_CASE("short report") {
  const auto test_case = GENERATE(
      ShortReportTestCase(
          {"crc changes", "crc_0.xml", "crc_1.xml", "crc_changes_short_diff"}),
      ShortReportTestCase({"only crc changes", "crc_only_0.xml",
                           "crc_only_1.xml", "crc_only_changes_short_diff"}),
      ShortReportTestCase({"offset changes", "offset_0.xml", "offset_1.xml",
                           "offset_changes_short_diff"}),
      ShortReportTestCase(
          {"symbols added and removed", "added_removed_symbols_0.xml",
           "added_removed_symbols_1.xml", "added_removed_symbols_short_diff"}),
      ShortReportTestCase({"symbols added and removed only",
                           "added_removed_symbols_only_0.xml",
                           "added_removed_symbols_only_1.xml",
                           "added_removed_symbols_only_short_diff"}));

  SECTION(test_case.name) {
    // Read inputs.
    stg::Graph graph;
    const auto id0 = stg::abixml::Read(graph, filename_to_path(test_case.xml0));
    const auto id1 = stg::abixml::Read(graph, filename_to_path(test_case.xml1));

    // Compute differences.
    stg::State state{graph, {}};
    const auto& [equals, comparison] = stg::Compare(state, id0, id1);

    // Write SHORT reports.
    std::stringstream output;
    if (comparison) {
      stg::NameCache names;
      stg::ReportingOptions options{stg::OutputFormat::SHORT, 2};
      stg::Reporting reporting{graph, state.outcomes, options, names};
      Report(reporting, *comparison, output);
    }

    // Check comparison outcome and report output.
    CHECK(equals == false);
    std::ifstream expected_output_file(
        filename_to_path(test_case.expected_output));
    std::ostringstream expected_output;
    expected_output << expected_output_file.rdbuf();
    CHECK(output.str() == expected_output.str());
  }
}
