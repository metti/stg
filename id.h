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
// Author: Giuliano Procida

#ifndef STG_ID_H_
#define STG_ID_H_

#include <cstddef>
#include <functional>
#include <ostream>

namespace stg {

// A wrapped (for type safety) array index.
struct Id {
  explicit Id(size_t ix) : ix_(ix) {}
  size_t ix_;
  bool operator==(const Id& other) const { return ix_ == other.ix_; }
  bool operator!=(const Id& other) const { return !operator==(other); }
};

inline std::ostream& operator<<(std::ostream& os, Id id) {
  return os << '<' << id.ix_ << '>';
}

}  // namespace stg

namespace std {

template<>
struct hash<stg::Id> {
  size_t operator()(const stg::Id& id) const noexcept { return id.ix_; }
};

}  // namespace std

#endif  // STG_ID_H_
