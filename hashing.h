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
// Author: Siddharth Nayyar

#ifndef STG_HASHING_H_
#define STG_HASHING_H_

#include <cstdint>
#include <string>

namespace stg {

struct Hash {
  // Hash 64 bits by splitting, hashing and combining.
  constexpr uint32_t operator()(uint64_t x) const {
    uint32_t lo = x;
    uint32_t hi = x >> 32;
    return (*this)(lo, hi);
  }

  // See https://github.com/skeeto/hash-prospector.
  constexpr uint32_t operator()(uint32_t x) const {
    x ^= x >> 16;
    x *= 0x21f0aaad;
    x ^= x >> 15;
    x *= 0xd35a2d97;
    x ^= x >> 15;
    return x;
  }

  // Hash 8 bits by zero extending to 32 bits.
  constexpr uint32_t operator()(char x) const {
    return (*this)(static_cast<uint32_t>(static_cast<unsigned char>(x)));
  }

  // 32-bit FNV-1a. See https://wikipedia.org/wiki/Fowler-Noll-Vo_hash_function.
  constexpr uint32_t operator()(const std::string& x) const {
    uint32_t h = 0x811c9dc5;
    for (auto ch : x) {
      h ^= static_cast<unsigned char>(ch);
      h *= 0x01000193;
    }
    return h;
  }

  // Reverse order Boost hash_combine (must be used with good hashes).
  template <typename Arg, typename... Args>
  constexpr uint32_t operator()(Arg arg, Args... args) const {
    auto seed = (*this)(args...);
    return seed ^ ((*this)(arg) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
  }
};

}  // namespace stg

#endif  // STG_HASHING_H_
