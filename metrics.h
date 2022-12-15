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

#ifndef STG_METRICS_H_
#define STG_METRICS_H_

#include <cstdint>
#include <ctime>
#include <ostream>
#include <variant>
#include <vector>

namespace stg {

struct Nanoseconds {
  explicit Nanoseconds(uint64_t ns) : ns(ns) {}
  uint64_t ns;
};

struct Metric {
  const char* name;
  std::variant<std::monostate, Nanoseconds, size_t> value;
};

using Metrics = std::vector<Metric>;

void Report(const Metrics& metrics, std::ostream& os);

// These objects only record values on destruction, so scope them!

class Time {
 public:
  Time(Metrics& metrics, const char* name);
  ~Time();

 private:
  Metrics& metrics_;
  size_t index_;
  struct timespec start_;
};

class Counter {
 public:
  Counter(Metrics& metrics, const char* name);
  ~Counter();

  Counter& operator=(size_t x) {
    value_ = x;
    return *this;
  }

  Counter& operator+=(size_t x) {
    value_ += x;
    return *this;
  }

  Counter& operator++() {
    ++value_;
    return *this;
  }

 private:
  Metrics& metrics_;
  size_t index_;
  size_t value_;
};

}  // namespace stg

#endif  // STG_METRICS_H_
