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

#ifndef STG_TIMING_H_
#define STG_TIMING_H_

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <vector>

namespace stg {

struct Timing { const char* what; uint64_t ns; };
using Times = std::vector<Timing>;

class Time {
 public:
  Time(Times& times, const char* what) : times_(times), index_(times.size()) {
    clock_gettime(CLOCK_MONOTONIC, &start_);
    times_.push_back(Timing{what, 0});
  }

  ~Time() {
    struct timespec finish;
    clock_gettime(CLOCK_MONOTONIC, &finish);
    auto seconds = finish.tv_sec - start_.tv_sec;
    auto nanos = finish.tv_nsec - start_.tv_nsec;
    times_[index_].ns = seconds * 1'000'000'000 + nanos;
  }

  static void report(const Times& times, std::ostream& os) {
    for (const auto& [what, ns] : times) {
      auto millis = ns / 1'000'000;
      auto nanos = ns % 1'000'000;
      os << what << ": " << millis << '.' << std::setfill('0')
         << std::setw(6) << nanos << std::setfill(' ') << " ms\n";
    }
  }

 private:
  Times& times_;
  size_t index_;
  struct timespec start_;
};

}  // namespace stg

#endif  // STG_TIMING_H_
