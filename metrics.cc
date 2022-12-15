// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2021-2022 Google LLC
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

#include <iomanip>

namespace stg {

std::ostream& operator<<(std::ostream& os, std::monostate) {
  return os << "<incomplete>";
}

std::ostream& operator<<(std::ostream& os, const Nanoseconds& value) {
  const auto millis = value.ns / 1'000'000;
  const auto nanos = value.ns % 1'000'000;
  // fill needs to be reset; width is reset automatically
  return os << millis << '.' << std::setfill('0') << std::setw(6) << nanos
            << std::setfill(' ') << " ms";
}

void Report(const Metrics& metrics, std::ostream& os) {
  for (const auto& metric : metrics) {
    std::visit([&](auto&& value) {
      os << metric.name << ": " << value << '\n';
    }, metric.value);
  }
}

Time::Time(Metrics& metrics, const char* name)
    : metrics_(metrics), index_(metrics.size()) {
  clock_gettime(CLOCK_MONOTONIC, &start_);
  metrics_.push_back(Metric{name, std::monostate()});
}

Time::~Time() {
  struct timespec finish;
  clock_gettime(CLOCK_MONOTONIC, &finish);
  const auto seconds = finish.tv_sec - start_.tv_sec;
  const auto nanos = finish.tv_nsec - start_.tv_nsec;
  metrics_[index_].value.emplace<1>(seconds * 1'000'000'000 + nanos);
}

}  // namespace stg
