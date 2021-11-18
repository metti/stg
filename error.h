// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2021 Google LLC
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

#ifndef STG_ERROR_H_
#define STG_ERROR_H_

#include <iostream>
#include <optional>
#include <sstream>

namespace stg {

#ifdef FOR_FUZZING
class Exception : std::exception {};

class Check {
 public:
  Check(bool ok) {
    if (!ok)
      throw Exception();
  }
  template <typename T>
  Check& operator<<(const T&) {
    return *this;
  }
};
#else
class Check {
 public:
  explicit Check(bool ok)
      : os_(ok ? std::optional<std::ostringstream>()
               : std::make_optional<std::ostringstream>()) {}
  ~Check() {
    if (os_) {
      *os_ << '\n';
      std::cerr << os_->str();
      exit(1);
    }
  }
  template <typename T>
  Check& operator<<(const T& t) {
    if (os_)
      *os_ << t;
    return *this;
  }

 private:
  std::optional<std::ostringstream> os_;
};
#endif

inline Check Die() {
  return Check(false);
}

}  // namespace stg

#endif  // STG_ERROR_H_
