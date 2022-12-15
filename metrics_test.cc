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

#include "metrics.h"

#include <cstdint>
#include <sstream>
#include <vector>

#include <catch2/catch.hpp>

namespace Test {

TEST_CASE("empty") {
  stg::Metrics metrics;
  std::ostringstream os;
  stg::Report(metrics, os);
  CHECK(os.str().empty());
}

TEST_CASE("incomplete") {
  stg::Metrics metrics;
  std::ostringstream os;
  stg::Time x(metrics, "a");
  stg::Counter y(metrics, "b");
  stg::Report(metrics, os);
  const std::string expected = "a: <incomplete>\nb: <incomplete>\n";
  CHECK(os.str() == expected);
}

TEST_CASE("times") {
  stg::Metrics metrics;
  const size_t count = 20;
  std::vector<stg::Time> times;
  for (size_t i = 0; i < count; ++i) {
    times.emplace_back(metrics, "name");
  }
  for (size_t i = 0; i < count; ++i) {
    times.pop_back();
  }
  std::ostringstream os;
  stg::Report(metrics, os);
  std::istringstream is(os.str());
  const std::string name = "name:";
  const std::string ms = "ms";
  size_t index = 0;
  double last_time = 0.0;
  while (is && index < count) {
    std::string first;
    double time;
    std::string second;
    is >> first >> time >> second;
    CHECK(first == name);
    if (last_time != 0.0) {
      CHECK(time < last_time);
    }
    CHECK(second == ms);
    last_time = time;
    ++index;
  }
  CHECK(index == count);
  std::string junk;
  is >> junk;
  CHECK(junk.empty());
  CHECK(is.eof());
}

TEST_CASE("counters") {
  stg::Metrics metrics;
  {
    stg::Counter a(metrics, "a");
    stg::Counter b(metrics, "b");
    stg::Counter c(metrics, "c");
    stg::Counter d(metrics, "d");
    stg::Counter e(metrics, "e");
    c = 17;
    ++b;
    ++b;
    e = 1;
    a = 3;
    c += 2;
  }
  std::ostringstream os;
  Report(metrics, os);
  const std::string expected = "a: 3\nb: 2\nc: 19\nd: 0\ne: 1\n";
  CHECK(os.str() == expected);
}

}  // namespace Test
