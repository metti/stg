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
// Author: Aleksei Vetrov

#ifndef STG_ELF_LOADER_H_
#define STG_ELF_LOADER_H_

#include <string>
#include <string_view>

struct Elf;
struct Elf_Scn;

namespace stg {
namespace elf {

class ElfLoader final {
 public:
  ElfLoader(const std::string& path);
  ElfLoader(const ElfLoader&) = delete;
  ElfLoader& operator=(const ElfLoader&) = delete;
  ~ElfLoader();

  std::string_view GetBtfRawData() const;

 private:
  Elf_Scn* GetBtfSection() const;

  std::string path_;
  int fd_;
  Elf* elf_;
};

}  // namespace elf
}  // namespace stg

#endif  // STG_ELF_LOADER_H_