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

#include "dwarf_wrappers.h"

#include <elf.h>
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "error.h"

namespace stg {
namespace dwarf {

namespace {

static const Dwfl_Callbacks kDwflCallbacks = {
    .find_elf = nullptr,
    .find_debuginfo = dwfl_standard_find_debuginfo,
    .section_address = dwfl_offline_section_address,
    .debuginfo_path = nullptr};

constexpr int kReturnOk = 0;
constexpr int kReturnNoEntry = 1;

std::optional<Dwarf_Attribute> GetAttribute(Dwarf_Die* die, int attribute) {
  // Create an optional with default-initialized value already inside
  std::optional<Dwarf_Attribute> result(std::in_place);
  // "integrate" automatically resolves DW_AT_abstract_origin and
  // DW_AT_specification references, fetching the attribute from the linked DIE.
  //
  // libdw has infinite loop protection, as it stops after 16 dereferences.
  if (!dwarf_attr_integrate(die, attribute, &result.value())) {
    result.reset();
  }
  return result;
}

void CheckOrDwflError(bool condition, const char* caller) {
  if (!condition) {
    int dwfl_error = dwfl_errno();
    const char* errmsg = dwfl_errmsg(dwfl_error);
    if (errmsg == nullptr) {
      // There are some cases when DWFL fails to produce an error message.
      Die() << caller << " returned error code 0x" << std::hex << dwfl_error;
    }
    Die() << caller << " returned error: " << errmsg;
  }
}

}  // namespace

Handler::Handler(const std::string& path) : dwfl_(dwfl_begin(&kDwflCallbacks)) {
  CheckOrDwflError(dwfl_.get(), "dwfl_begin");
  // Add data to process to dwfl
  dwfl_module_ =
      dwfl_report_offline(dwfl_.get(), path.c_str(), path.c_str(), -1);
  InitialiseDwarf();
}

Handler::Handler(char* data, size_t size) : dwfl_(dwfl_begin(&kDwflCallbacks)) {
  CheckOrDwflError(dwfl_.get(), "dwfl_begin");
  // Add data to process to dwfl
  dwfl_module_ = dwfl_report_offline_memory(dwfl_.get(), "<memory>", "<memory>",
                                            data, size);
  InitialiseDwarf();
}

void Handler::InitialiseDwarf() {
  CheckOrDwflError(dwfl_.get(), "dwfl_report_offline");
  // Finish adding files to dwfl and process them
  CheckOrDwflError(dwfl_report_end(dwfl_.get(), nullptr, nullptr) == kReturnOk,
                   "dwfl_report_end");
  GElf_Addr loadbase = 0;  // output argument for dwfl, unused by us
  dwarf_ = dwfl_module_getdwarf(dwfl_module_, &loadbase);
  CheckOrDwflError(dwarf_, "dwfl_module_getdwarf");
}

std::vector<Entry> Handler::GetCompilationUnits() {
  std::vector<Entry> result;
  Dwarf_Off offset = 0;
  while (true) {
    Dwarf_Off next_offset;
    size_t header_size = 0;
    int return_code =
        dwarf_next_unit(dwarf_, offset, &next_offset, &header_size, nullptr,
                        nullptr, nullptr, nullptr, nullptr, nullptr);
    Check(return_code == kReturnOk || return_code == kReturnNoEntry)
        << "dwarf_next_unit returned error";
    if (return_code == kReturnNoEntry) {
      break;
    }
    result.push_back({});
    Check(dwarf_offdie(dwarf_, offset + header_size, &result.back().die))
        << "dwarf_offdie returned error";

    offset = next_offset;
  }
  return result;
}

std::vector<Entry> Entry::GetChildren() {
  Entry child;
  int return_code = dwarf_child(&die, &child.die);
  Check(return_code == kReturnOk || return_code == kReturnNoEntry)
      << "dwarf_child returned error";
  std::vector<Entry> result;
  while (return_code == kReturnOk) {
    result.push_back(child);
    return_code = dwarf_siblingof(&child.die, &child.die);
    Check(return_code == kReturnOk || return_code == kReturnNoEntry)
        << "dwarf_siblingof returned error";
  }
  return result;
}

int Entry::GetTag() {
  return dwarf_tag(&die);
}

Dwarf_Off Entry::GetOffset() {
  return dwarf_dieoffset(&die);
}

std::optional<std::string> Entry::MaybeGetString(uint32_t attribute) {
  std::optional<std::string> result;
  auto dwarf_attribute = GetAttribute(&die, attribute);
  if (!dwarf_attribute) {
    return result;
  }

  const char* value = dwarf_formstring(&dwarf_attribute.value());
  Check(value) << "dwarf_formstring returned error";
  result.emplace(value);
  return result;
}

std::optional<uint64_t> Entry::MaybeGetUnsignedConstant(
    uint32_t attribute) {
  std::optional<uint64_t> result;
  auto dwarf_attribute = GetAttribute(&die, attribute);
  if (!dwarf_attribute) {
    return result;
  }

  // Place default-initialized value inside to be filled with dwarf_formudata
  result.emplace();
  Check(dwarf_formudata(&dwarf_attribute.value(), &result.value()) == kReturnOk)
      << "dwarf_formudata returned error";
  return result;
}

std::optional<Entry> Entry::MaybeGetReference(uint32_t attribute) {
  std::optional<Entry> result;
  auto dwarf_attribute = GetAttribute(&die, attribute);
  if (!dwarf_attribute) {
    return result;
  }

  result.emplace();
  Check(dwarf_formref_die(&dwarf_attribute.value(), &result->die))
      << "dwarf_formref_die returned error\n";
  return result;
}

}  // namespace dwarf
}  // namespace stg
