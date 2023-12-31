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

#include "error.h"

#include <catch2/catch.hpp>

namespace Test {

TEST_CASE("Check") {
  CHECK_NOTHROW(stg::Check(true) << "phew");
  CHECK_THROWS(stg::Check(false) << "oh dear");
}

TEST_CASE("Die") {
  CHECK_THROWS(stg::Die() << "Mr Bond");
}

}  // namespace Test
