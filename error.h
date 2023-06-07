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

#ifndef STG_ERROR_H_
#define STG_ERROR_H_

#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace stg {

class Exception : public std::exception {
 public:
  explicit Exception(const std::string& message) : message_(message) {}

  const char* what() const noexcept(true) final {
    return message_.c_str();
  }

 private:
  const std::string message_;
};

class Check {
 public:
  explicit Check(bool ok)
      : os_(ok ? std::optional<std::ostringstream>()
               : std::make_optional<std::ostringstream>()) {}
  ~Check() noexcept(false) {
    if (os_) {
      throw Exception(os_->str());
    }
  }

  template <typename T>
  Check& operator<<(const T& t) {
    if (os_) {
      *os_ << t;
    }
    return *this;
  }

 private:
  std::optional<std::ostringstream> os_;
};

class Die {
 public:
  [[noreturn]] ~Die() noexcept(false) {
    throw Exception(os_.str());
  }

  template <typename T>
  Die& operator<<(const T& t) {
    os_ << t;
    return *this;
  }

 private:
  std::ostringstream os_;
};

class Warn {
 public:
  ~Warn() {
    std::cerr << "warning: " << os_.str() << '\n';
  }

  template <typename T>
  Warn& operator<<(const T& t) {
    os_ << t;
    return *this;
  }

 private:
  std::ostringstream os_;
};

inline std::string ErrnoToString(int error) {
  return std::system_error(error, std::generic_category()).what();
}

}  // namespace stg

#endif  // STG_ERROR_H_
