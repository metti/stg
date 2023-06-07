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

#ifndef STG_CRC_H_
#define STG_CRC_H_

#include <ios>
#include <ostream>

namespace stg {

struct CRC {
  uint64_t number;
};

inline bool operator==(CRC crc1, CRC crc2) {
  return crc1.number == crc2.number;
}

inline bool operator!=(CRC crc1, CRC crc2) {
  return crc1.number != crc2.number;
}

inline std::ostream& operator<<(std::ostream& os, CRC crc) {
  return os << "0x" << std::hex << crc.number;
}

}  // namespace stg

#endif  // STG_CRC_H_
