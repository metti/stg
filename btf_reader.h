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
// Author: Maria Teguiani
// Author: Giuliano Procida

#ifndef STG_BTF_READER_H_
#define STG_BTF_READER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <abg-fwd.h>  // for symtab_sptr
#include <abg-ir.h>  // for environment
#include "stg.h"
#include <linux/btf.h>

namespace stg {
namespace btf {

// BTF Specification: https://www.kernel.org/doc/html/latest/bpf/btf.html
class Structs : public Graph {
 public:
  Structs(const char* start, size_t size,
          std::unique_ptr<abigail::ir::environment> env,
          const abigail::symtab_reader::symtab_sptr tab,
          const bool verbose = false);
  const Type& GetSymbols() const { return *types_[symbols_index_].get(); }

 private:
  struct MemoryRange {
    const char* start;
    const char* limit;
    bool Empty() const;
    template <typename T> const T* Pull(size_t count = 1);
  };

  MemoryRange string_section_;
  const std::unique_ptr<abigail::ir::environment> env_;
  const abigail::symtab_reader::symtab_sptr tab_;
  const bool verbose_;

  std::vector<std::unique_ptr<Type>> types_;
  std::optional<size_t> void_type_id_;
  std::optional<size_t> variadic_type_id_;
  std::unordered_map<uint32_t, size_t> type_ids_;

  size_t symbols_index_;
  std::map<std::string, Id> btf_symbol_types_;

  size_t GetVoidIndex();
  size_t GetVariadicIndex();
  size_t GetIndex(uint32_t btf_index);
  Id GetId(uint32_t btf_index);
  Id GetParameterId(uint32_t btf_index);

  void PrintHeader(const btf_header* header) const;
  void BuildTypes(MemoryRange memory);
  void BuildOneType(const btf_type* t, uint32_t btf_index,
                    MemoryRange& memory);
  void BuildSymbols();
  std::vector<Id> BuildMembers(
      bool kflag, const btf_member* members, size_t vlen);
  Enumeration::Enumerators BuildEnums(
      const struct btf_enum* enums, size_t vlen);
  std::vector<Parameter> BuildParams(const struct btf_param* params,
                                     size_t vlen);
  std::string GetName(uint32_t name_off);

  static void PrintStrings(MemoryRange memory);
};

std::unique_ptr<Structs> ReadFile(
    const std::string& path, bool verbose = false);

}  // namespace btf
}  // namespace stg

#endif  // STG_BTF_READER_H_
