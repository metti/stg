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

#include <dwarf.h>
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
#include <utility>
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

std::optional<Dwarf_Attribute> GetAttribute(Dwarf_Die* die,
                                            uint32_t attribute) {
  // Create an optional with default-initialized value already inside
  std::optional<Dwarf_Attribute> result(std::in_place);
  // "integrate" automatically resolves DW_AT_abstract_origin and
  // DW_AT_specification references, fetching the attribute from the linked DIE.
  //
  // libdw has infinite loop protection, as it stops after 16 dereferences.
  // TODO: don't use dwarf_attr_integrate by default
  if (!dwarf_attr_integrate(die, attribute, &result.value())) {
    result.reset();
  }
  return result;
}

// Get the attribute directly from DIE without following DW_AT_specification and
// DW_AT_abstract_origin references.
std::optional<Dwarf_Attribute> GetDirectAttribute(Dwarf_Die* die,
                                                  uint32_t attribute) {
  // Create an optional with default-initialized value already inside
  std::optional<Dwarf_Attribute> result(std::in_place);
  if (!dwarf_attr(die, attribute, &result.value())) {
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
      Die() << caller << " returned error code " << Hex(dwfl_error);
    }
    Die() << caller << " returned error: " << errmsg;
  }
}

std::optional<uint64_t> MaybeGetUnsignedOperand(const Dwarf_Op& operand) {
  switch (operand.atom) {
    case DW_OP_addr:
    case DW_OP_const1u:
    case DW_OP_const2u:
    case DW_OP_const4u:
    case DW_OP_const8u:
    case DW_OP_constu:
      return operand.number;
    case DW_OP_const1s:
    case DW_OP_const2s:
    case DW_OP_const4s:
    case DW_OP_const8s:
    case DW_OP_consts:
      if (static_cast<int64_t>(operand.number) < 0) {
        // Atom is not an unsigned constant
        return std::nullopt;
      }
      return operand.number;
    case DW_OP_lit0...DW_OP_lit31:
      return operand.atom - DW_OP_lit0;
    default:
      return std::nullopt;
  }
}

struct Expression {
  const Dwarf_Op& operator[](size_t i) const {
    return atoms[i];
  }

  Dwarf_Op* atoms = nullptr;
  size_t length = 0;
};

Expression GetExpression(Dwarf_Attribute& attribute) {
  Expression result;

  Check(dwarf_getlocation(&attribute, &result.atoms, &result.length) ==
        kReturnOk) << "dwarf_getlocation returned error";
  Check(result.atoms != nullptr && result.length > 0)
      << "dwarf_getlocation returned empty expression";
  return result;
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

  // Check if ELF can be opened from input data, because DWFL couldn't handle
  // memory, that is not ELF.
  // TODO: remove this workaround
  Elf* elf = elf_memory(data, size);
  Check(elf != nullptr) << "Input data is not ELF";
  elf_end(elf);

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

Elf* Handler::GetElf() {
  GElf_Addr loadbase = 0;  // output argument for dwfl, unused by us
  Elf* elf = dwfl_module_getelf(dwfl_module_, &loadbase);
  CheckOrDwflError(elf, "dwfl_module_getelf");
  return elf;
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
  Check(value != nullptr) << "dwarf_formstring returned error";
  result.emplace(value);
  return result;
}

std::optional<std::string> Entry::MaybeGetDirectString(uint32_t attribute) {
  std::optional<std::string> result;
  auto dwarf_attribute = GetDirectAttribute(&die, attribute);
  if (!dwarf_attribute) {
    return result;
  }

  const char* value = dwarf_formstring(&dwarf_attribute.value());
  Check(value != nullptr) << "dwarf_formstring returned error";
  result.emplace(value);
  return result;
}

std::optional<uint64_t> Entry::MaybeGetUnsignedConstant(uint32_t attribute) {
  auto dwarf_attribute = GetAttribute(&die, attribute);
  if (!dwarf_attribute) {
    return {};
  }

  uint64_t value;
  if (dwarf_formudata(&dwarf_attribute.value(), &value) != kReturnOk) {
    Die() << "dwarf_formudata returned error";
  }
  return value;
}

bool Entry::GetFlag(uint32_t attribute) {
  bool result = false;
  auto dwarf_attribute = (attribute == DW_AT_declaration)
                             ? GetDirectAttribute(&die, attribute)
                             : GetAttribute(&die, attribute);
  if (!dwarf_attribute) {
    return result;
  }

  Check(dwarf_formflag(&dwarf_attribute.value(), &result) == kReturnOk)
      << "dwarf_formflag returned error";
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
      << "dwarf_formref_die returned error";
  return result;
}

namespace {

std::optional<uint64_t> GetAddressFromLocation(Dwarf_Attribute& attribute) {
  const auto expression = GetExpression(attribute);

  Dwarf_Attribute result_attribute;
  if (dwarf_getlocation_attr(&attribute, expression.atoms, &result_attribute) ==
      kReturnOk) {
    uint64_t address;
    Check(dwarf_formaddr(&result_attribute, &address) == kReturnOk)
        << "dwarf_formaddr returned error";
    return address;
  }
  if (expression.length == 1 && expression[0].atom == DW_OP_addr) {
    // DW_OP_addr is unsupported by dwarf_getlocation_attr, so we need to
    // manually extract the address from expression.
    return expression[0].number;
  }
  if (expression.length == 2 &&
      (expression[1].atom == DW_OP_GNU_push_tls_address ||
       expression[1].atom == DW_OP_form_tls_address)) {
    // We don't handle TLS location expressions.
    return {};
  }

  Die() << "Unsupported data location expression";
}

}  // namespace

std::optional<uint64_t> Entry::MaybeGetAddress(uint32_t attribute) {
  auto dwarf_attribute = GetAttribute(&die, attribute);
  if (!dwarf_attribute) {
    return {};
  }
  if (attribute == DW_AT_location) {
    return GetAddressFromLocation(*dwarf_attribute);
  }

  uint64_t addr;
  Check(dwarf_formaddr(&dwarf_attribute.value(), &addr) == kReturnOk)
      << "dwarf_formaddr returned error";
  return addr;
}

std::optional<uint64_t> Entry::MaybeGetMemberByteOffset() {
  auto attribute = GetAttribute(&die, DW_AT_data_member_location);
  if (!attribute) {
    return {};
  }

  uint64_t offset;
  // Try to interpret attribute as an unsigned integer constant
  if (dwarf_formudata(&attribute.value(), &offset) == kReturnOk) {
    return offset;
  }

  // Parse location expression
  const auto expression = GetExpression(attribute.value());

  // Parse virtual base classes offset, which looks like this:
  //   [0] = DW_OP_dup
  //   [1] = DW_OP_deref
  //   [2] = constant operand
  //   [3] = DW_OP_minus
  //   [4] = DW_OP_deref
  //   [5] = DW_OP_plus
  // This form is not in the standard, but hardcoded in compilers:
  //   * https://github.com/llvm/llvm-project/blob/release/17.x/llvm/lib/CodeGen/AsmPrinter/DwarfUnit.cpp#L1611
  //   * https://github.com/gcc-mirror/gcc/blob/releases/gcc-13/gcc/dwarf2out.cc#L20029
  if (expression.length == 6 &&
      expression[0].atom == DW_OP_dup &&
      expression[1].atom == DW_OP_deref &&
      expression[3].atom == DW_OP_minus &&
      expression[4].atom == DW_OP_deref &&
      expression[5].atom == DW_OP_plus) {
    const auto byte_offset = MaybeGetUnsignedOperand(expression[2]);
    if (byte_offset) {
      return byte_offset;
    }
  }

  Die() << "Unsupported member offset expression, " << Hex(GetOffset());
}

std::optional<uint64_t> Entry::MaybeGetCount() {
  auto dwarf_attribute = GetAttribute(&die, DW_AT_count);
  if (!dwarf_attribute) {
    return {};
  }

  uint64_t value;
  if (dwarf_formudata(&dwarf_attribute.value(), &value) != kReturnOk) {
    // All errors are interpreted as "value of DW_AT_count is not a constant"
    // and ignored. There is no stable API in libdw for checking the exact error
    // reason.
    // TODO: implement clean solution for separating "not a
    // constant" errors from other errors.
    return {};
  }
  return value;
}

}  // namespace dwarf
}  // namespace stg
