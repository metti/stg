// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2022-2023 Google LLC
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

#include "dwarf_processor.h"

#include <dwarf.h>

#include <ios>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dwarf_wrappers.h"
#include "error.h"
#include "graph.h"

namespace stg {
namespace dwarf {

namespace {

std::string EntryToString(Entry& entry) {
  std::ostringstream os;
  os << "DWARF entry <0x" << std::hex << entry.GetOffset() << ">";
  return os.str();
}

std::optional<std::string> MaybeGetName(Entry& entry) {
  return entry.MaybeGetString(DW_AT_name);
}

std::string GetName(Entry& entry) {
  auto result = MaybeGetName(entry);
  if (!result.has_value()) {
    Die() << "Name was not found for " << EntryToString(entry);
  }
  return std::move(*result);
}

std::string GetNameOrEmpty(Entry& entry) {
  auto result = MaybeGetName(entry);
  if (!result.has_value()) {
    return std::string();
  }
  return std::move(*result);
}

size_t GetBitSize(Entry& entry) {
  if (auto byte_size = entry.MaybeGetUnsignedConstant(DW_AT_byte_size)) {
    return *byte_size * 8;
  } else if (auto bit_size = entry.MaybeGetUnsignedConstant(DW_AT_bit_size)) {
    return *bit_size;
  }
  Die() << "Bit size was not found for " << EntryToString(entry);
}

size_t GetByteSize(Entry& entry) {
  if (auto byte_size = entry.MaybeGetUnsignedConstant(DW_AT_byte_size)) {
    return *byte_size;
  } else if (auto bit_size = entry.MaybeGetUnsignedConstant(DW_AT_bit_size)) {
    // Round up bit_size / 8 to get minimal needed storage size in bytes.
    return (*bit_size + 7) / 8;
  }
  Die() << "Byte size was not found for " << EntryToString(entry);
}

Primitive::Encoding GetEncoding(Entry& entry) {
  auto dwarf_encoding = entry.MaybeGetUnsignedConstant(DW_AT_encoding);
  if (!dwarf_encoding) {
    Die() << "Encoding was not found for " << EntryToString(entry);
  }
  switch (*dwarf_encoding) {
    case DW_ATE_boolean:
      return Primitive::Encoding::BOOLEAN;
    case DW_ATE_complex_float:
      return Primitive::Encoding::COMPLEX_NUMBER;
    case DW_ATE_float:
      return Primitive::Encoding::REAL_NUMBER;
    case DW_ATE_signed:
      return Primitive::Encoding::SIGNED_INTEGER;
    case DW_ATE_signed_char:
      return Primitive::Encoding::SIGNED_CHARACTER;
    case DW_ATE_unsigned:
      return Primitive::Encoding::UNSIGNED_INTEGER;
    case DW_ATE_unsigned_char:
      return Primitive::Encoding::UNSIGNED_CHARACTER;
    case DW_ATE_UTF:
      return Primitive::Encoding::UTF;
    default:
      Die() << "Unknown encoding 0x" << std::hex << *dwarf_encoding << " for "
            << EntryToString(entry);
  }
}

std::optional<Entry> MaybeGetReferredType(Entry& entry) {
  return entry.MaybeGetReference(DW_AT_type);
}

Entry GetReferredType(Entry& entry) {
  auto result = MaybeGetReferredType(entry);
  if (!result.has_value()) {
    Die() << "Type reference was not found in " << EntryToString(entry);
  }
  return std::move(*result);
}

size_t GetNumberOfElements(Entry& entry) {
  // DWARF standard says, that array dimensions could be an entry with
  // either DW_TAG_subrange_type or DW_TAG_enumeration_type. However, this
  // code supports only the DW_TAG_subrange_type.
  Check(entry.GetTag() == DW_TAG_subrange_type)
      << "Array's dimensions should be an entry of DW_TAG_subrange_type";
  std::optional<size_t> lower_bound_optional =
      entry.MaybeGetUnsignedConstant(DW_AT_lower_bound);
  Check(!lower_bound_optional.has_value() || *lower_bound_optional == 0)
      << "Non-zero DW_AT_lower_bound is not supported";
  std::optional<size_t> upper_bound_optional =
      entry.MaybeGetUnsignedConstant(DW_AT_upper_bound);
  std::optional<size_t> number_of_elements_optional =
      entry.MaybeGetUnsignedConstant(DW_AT_count);
  if (upper_bound_optional && number_of_elements_optional) {
    Die() << "Both DW_AT_upper_bound and DW_AT_count given";
  } else if (upper_bound_optional) {
    return *upper_bound_optional + 1;
  } else if (number_of_elements_optional) {
    return *number_of_elements_optional;
  } else {
    // If a subrange has no DW_AT_count and no DW_AT_upper_bound attribue, its
    // size is unknown.
    return 0;
  }
}

// Calculate number of bits from the "beginning" of the containing entity to
// the "beginning" of the data member using DW_AT_bit_offset.
//
// "Number of bits from the beginning", depends on the definition of the
// "beginning", which is different for big- and little-endian architectures.
// However, DW_AT_bit_offset is defined from the high order bit of the storage
// unit to the high order bit of a field and is the same for both architectures.

// So this function converts DW_AT_bit_offset to the "number of bits from the
// beginning".
size_t CalculateBitfieldAdjustment(Entry& entry, size_t bit_size,
                             bool is_little_endian_binary) {
  if (bit_size == 0) {
    // bit_size == 0 marks that it is not a bit field. No adjustment needed.
    return 0;
  }
  auto container_byte_size = entry.MaybeGetUnsignedConstant(DW_AT_byte_size);
  auto bit_offset = entry.MaybeGetUnsignedConstant(DW_AT_bit_offset);
  Check(container_byte_size.has_value() && bit_offset.has_value())
      << "If member offset is defined as DW_AT_data_member_location, bit field "
         "should have DW_AT_byte_size and DW_AT_bit_offset";
  // The following structure will be used as an example in the explanations:
  // struct foo {
  //   uint16_t rest_of_the_struct;
  //   uint16_t x : 5;
  //   uint16_t y : 6;
  //   uint16_t z : 5;
  // };
  if (is_little_endian_binary) {
    // Compiler usualy packs bit fields starting with the least significant
    // bits, but DW_AT_bit_offset is counted from high to low bits:
    //
    // rest of the struct|<    container   >
    //    Container bits: 01234|56789A|BCDEF
    //  Bit-fields' bits: 01234|012345|01234
    //        bit_offset: <<<<B<<<<<<5<<<<<0
    //   bits from start: 0>>>>>5>>>>>>B>>>>
    //                    <x:5>|< y:6>|<z:5>
    //
    //   x.bit_offset: 11 (0xB) bits
    //   y.bit_offset: 5 bits
    //   z.bit_offset: 0 bits
    //
    // So we need to subtract bit_offset from the container bit size
    // (container_byte_size * 8) to inverse direction. Also we need to convert
    // from high- to low-order bit, because the field "begins" with low-order
    // bit. To do so we need to subtract field's bit size. Resulting formula is:
    //
    //   container_byte_size * 8 - bit_offset - bit_size
    //
    // If we try it on example, we get correct values:
    //   x: 2 * 8 - 11 - 5 = 0
    //   y: 2 * 8 - 5 - 6 = 5
    //   z: 2 * 8 - 0 - 5 = 11 (0xB)
    return *container_byte_size * 8 - *bit_offset - bit_size;
  }
  // Big-endian orders begins with high-order bit and the bit_offset is from the
  // high order bit:
  //
  // rest of the struct|<    container   >
  //    Container bits: FEDCB|A98765|43210
  //  Bit-fields' bits: 43210|543210|43210
  //        bit_offset: 0>>>>>5>>>>>>B>>>>
  //   bits from start: 0>>>>>5>>>>>>B>>>>
  //                    <x:5>|< y:6>|<z:5>
  //
  // So we just return bit_offset.
  return *bit_offset;
}

// Calculate the number of bits from the beginning of the structure to the
// beginning of the data member.
size_t GetDataBitOffset(Entry& entry, size_t bit_size,
                        bool is_little_endian_binary) {
  // Offset may be represented either by DW_AT_data_bit_offset (in bits) or by
  // DW_AT_data_member_location (in bytes).
  if (auto data_bit_offset =
          entry.MaybeGetUnsignedConstant(DW_AT_data_bit_offset)) {
    // DW_AT_data_bit_offset contains what this function needs for any type
    // of member (bitfield or not) on architecture of any endianness.
    return *data_bit_offset;
  } else if (auto byte_offset = entry.MaybeGetMemberByteOffset()) {
    // DW_AT_data_member_location contains offset in bytes.
    const size_t bit_offset = *byte_offset * 8;
    // But there can be offset part, coming from DW_AT_bit_offset. DWARF 5
    // standard requires to use DW_AT_data_bit_offset in this case, but a lot
    // of binaries still use combination of DW_AT_data_member_location and
    // DW_AT_bit_offset.
    const size_t bitfield_adjusment =
        CalculateBitfieldAdjustment(entry, bit_size, is_little_endian_binary);
    return bit_offset + bitfield_adjusment;
  }
  Die() << "Member has no DW_AT_data_bit_offset or DW_AT_data_member_location";
}

}  // namespace

// Transforms DWARF entries to STG.
class Processor {
 public:
  Processor(Graph& graph, Id void_id, Id variadic_id,
            bool is_little_endian_binary, Types& result)
      : graph_(graph),
        void_id_(void_id),
        variadic_id_(variadic_id),
        is_little_endian_binary_(is_little_endian_binary),
        result_(result) {}

  void Process(Entry& entry) {
    ++result_.processed_entries;
    auto tag = entry.GetTag();
    switch (tag) {
      case DW_TAG_array_type:
        ProcessArray(entry);
        break;
      case DW_TAG_enumeration_type:
        ProcessEnum(entry);
        break;
      case DW_TAG_class_type:
        ProcessStructUnion(entry, StructUnion::Kind::STRUCT);
        break;
      case DW_TAG_structure_type:
        ProcessStructUnion(entry, StructUnion::Kind::STRUCT);
        break;
      case DW_TAG_union_type:
        ProcessStructUnion(entry, StructUnion::Kind::UNION);
        break;
      case DW_TAG_member:
        ProcessMember(entry);
        break;
      case DW_TAG_pointer_type:
        ProcessReference<PointerReference>(
            entry, PointerReference::Kind::POINTER);
        break;
      case DW_TAG_reference_type:
        ProcessReference<PointerReference>(
            entry, PointerReference::Kind::LVALUE_REFERENCE);
        break;
      case DW_TAG_rvalue_reference_type:
        ProcessReference<PointerReference>(
            entry, PointerReference::Kind::RVALUE_REFERENCE);
        break;
      case DW_TAG_compile_unit:
        ProcessCompileUnit(entry);
        break;
      case DW_TAG_typedef:
        ProcessTypedef(entry);
        break;
      case DW_TAG_base_type:
        ProcessBaseType(entry);
        break;
      case DW_TAG_const_type:
        ProcessReference<Qualified>(entry, Qualifier::CONST);
        break;
      case DW_TAG_volatile_type:
        ProcessReference<Qualified>(entry, Qualifier::VOLATILE);
        break;
      case DW_TAG_restrict_type:
        ProcessReference<Qualified>(entry, Qualifier::RESTRICT);
        break;
      case DW_TAG_variable:
        ProcessVariable(entry);
        break;
      case DW_TAG_subroutine_type:
        // Standalone function type, for example, used in function pointers.
        ProcessFunction(entry);
        break;
      case DW_TAG_subprogram:
        // DWARF equivalent of ELF function symbol.
        ProcessFunction(entry);
        break;

      default:
        // TODO: die on unexpected tag, when this switch contains
        // all expected tags
        break;
    }
  }

  std::vector<Id> GetUnresolvedIds() {
    std::vector<Id> result;
    for (const auto& [offset, id] : id_map_) {
      if (!graph_.Is(id)) {
        result.push_back(id);
      }
    }
    return result;
  }

 private:
  void ProcessAllChildren(Entry& entry) {
    for (auto& child : entry.GetChildren()) {
      Process(child);
    }
  }

  void CheckNoChildren(Entry& entry) {
    if (!entry.GetChildren().empty()) {
      Die() << "Entry expected to have no children";
    }
  }

  void ProcessCompileUnit(Entry& entry) {
    ProcessAllChildren(entry);
  }

  void ProcessBaseType(Entry& entry) {
    CheckNoChildren(entry);
    const auto type_name = GetName(entry);
    const size_t bit_size = GetBitSize(entry);
    if (bit_size % 8) {
      Die() << "type '" << type_name << "' size is not a multiple of 8";
    }
    const size_t byte_size = bit_size / 8;
    AddProcessedNode<Primitive>(entry, type_name, GetEncoding(entry),
                                byte_size);
  }

  void ProcessTypedef(Entry& entry) {
    std::string type_name = GetName(entry);
    auto referred_type_id = GetIdForReferredType(MaybeGetReferredType(entry));
    AddProcessedNode<Typedef>(entry, std::move(type_name), referred_type_id);
  }

  template<typename Node, typename KindType>
  void ProcessReference(Entry& entry, KindType kind) {
    auto referred_type_id = GetIdForReferredType(MaybeGetReferredType(entry));
    AddProcessedNode<Node>(entry, kind, referred_type_id);
  }

  void ProcessStructUnion(Entry& entry, StructUnion::Kind kind) {
    // TODO: add scoping
    std::string name = GetNameOrEmpty(entry);

    if (entry.GetFlag(DW_AT_declaration)) {
      // It is expected to have only name and no children in declaration.
      // However, it is not guaranteed and we should do something if we find an
      // example.
      CheckNoChildren(entry);
      AddProcessedNode<StructUnion>(entry, kind, std::move(name));
      return;
    }

    auto byte_size = GetByteSize(entry);
    std::vector<Id> members;

    for (auto& child : entry.GetChildren()) {
      auto child_tag = child.GetTag();
      // All possible children of struct/class/union
      switch (child_tag) {
        case DW_TAG_member:
          members.push_back(GetIdForEntry(child));
          break;
        case DW_TAG_subprogram:
          // TODO: process methods
          break;
        case DW_TAG_inheritance:
          // TODO: process base classes
          break;
        case DW_TAG_structure_type:
        case DW_TAG_class_type:
        case DW_TAG_union_type:
        case DW_TAG_enumeration_type:
        case DW_TAG_typedef:
        case DW_TAG_variable:
          break;
        default:
          Die() << "Unexpected tag for child of struct/class/union: 0x"
                << std::hex << child_tag;
      }
      Process(child);
    }

    // TODO: support base classes
    // TODO: support methods
    AddProcessedNode<StructUnion>(entry, kind, std::move(name), byte_size,
                                  /* base_classes = */ std::vector<Id>{},
                                  /* methods = */ std::vector<Id>{},
                                  std::move(members));
  }

  void ProcessMember(Entry& entry) {
    std::string name = GetNameOrEmpty(entry);
    auto referred_type = GetReferredType(entry);
    auto referred_type_id = GetIdForEntry(referred_type);
    auto optional_bit_size = entry.MaybeGetUnsignedConstant(DW_AT_bit_size);
    // Member has DW_AT_bit_size if and only if it is bit field.
    // STG uses bit_size == 0 to mark that the member is not a bit field.
    Check(!optional_bit_size || *optional_bit_size > 0)
        << "DW_AT_bit_size should be a positive number";
    auto bit_size = optional_bit_size ? *optional_bit_size : 0;
    AddProcessedNode<Member>(
        entry, std::move(name), referred_type_id,
        GetDataBitOffset(entry, bit_size, is_little_endian_binary_), bit_size);
  }

  void ProcessArray(Entry& entry) {
    auto referred_type = GetReferredType(entry);
    auto referred_type_id = GetIdForEntry(referred_type);
    auto children = entry.GetChildren();
    // Multiple children in array describe multiple dimensions of this array.
    // For example, int[M][N] contains two children, M located in the first
    // child, N located in the second child. But in STG multidimensional arrays
    // are represented as chain of arrays: int[M][N] is array[M] of array[N] of
    // int.
    //
    // We need to chain children as types together in reversed order.
    // "referred_type_id" is updated every time to contain the top element in
    // the chain. Rightmost chldren refers to the original "referred_type_id".
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      auto& child = *it;
      // All subarrays except the first (last in the reversed order) are
      // attached to the corresponding child. First subarray (last in the
      // reversed order) is attached to the original entry itself.
      auto& entry_to_attach = (it + 1 == children.rend()) ? entry : child;
      // Update referred_type_id so next array in chain points there.
      referred_type_id = AddProcessedNode<Array>(
          entry_to_attach, GetNumberOfElements(child), referred_type_id);
    }
  }

  void ProcessEnum(Entry& entry) {
    std::string name = GetNameOrEmpty(entry);
    if (entry.GetFlag(DW_AT_declaration)) {
      // It is expected to have only name and no children in declaration.
      // However, it is not guaranteed and we should do something if we find an
      // example.
      CheckNoChildren(entry);
      AddProcessedNode<Enumeration>(entry, name);
      return;
    }
    auto underlying_type_id = GetIdForReferredType(MaybeGetReferredType(entry));
    auto children = entry.GetChildren();
    Enumeration::Enumerators enumerators;
    enumerators.reserve(children.size());
    for (auto& child : children) {
      Check(child.GetTag() == DW_TAG_enumerator)
          << "Enum expects child of DW_TAG_enumerator";
      std::string enumerator_name = GetName(child);
      // TODO: detect signedness of underlying type and call
      // an appropriate method.
      std::optional<size_t> value_optional =
          child.MaybeGetUnsignedConstant(DW_AT_const_value);
      Check(value_optional.has_value()) << "Enumerator should have value";
      // TODO: support both uint64_t and int64_t, depending on
      // signedness of underlying type.
      enumerators.emplace_back(enumerator_name,
                               static_cast<int64_t>(*value_optional));
    }
    AddProcessedNode<Enumeration>(entry, std::move(name), underlying_type_id,
                                  std::move(enumerators));
  }

  void ProcessVariable(Entry& entry) {
    // Skip:
    //  * anonymous variables (for example, anonymous union)
    //  * variables not visible outside of its enclosing compilation unit
    if (!entry.GetFlag(DW_AT_external)) {
      return;
    }
    std::optional<std::string> name_optional = MaybeGetName(entry);
    if (!name_optional) {
      return;
    }

    auto referred_type = GetReferredType(entry);
    auto referred_type_id = GetIdForEntry(referred_type);

    if (auto address = entry.MaybeGetAddress(DW_AT_location)) {
      // Only external variables with address are useful for ABI monitoring
      result_.symbols.push_back(Types::Symbol{
          .name = *name_optional,
          .linkage_name = entry.MaybeGetString(DW_AT_linkage_name),
          .address = *address,
          .id = referred_type_id});
    }
  }

  void ProcessFunction(Entry& entry) {
    auto return_type_id = GetIdForReferredType(MaybeGetReferredType(entry));

    std::vector<Id> parameters;
    for (auto& child : entry.GetChildren()) {
      auto child_tag = child.GetTag();
      if (child_tag == DW_TAG_formal_parameter) {
        auto child_type = GetReferredType(child);
        auto child_type_id = GetIdForEntry(child_type);
        parameters.push_back(child_type_id);
      } else if (child_tag == DW_TAG_unspecified_parameters) {
        // Note: C++ allows a single ... argument specification but C does not.
        // However, "extern int foo();" (note lack of "void" in parameters) in C
        // will produce the same DWARF as "extern int foo(...);" in C++.
        CheckNoChildren(child);
        parameters.push_back(variadic_id_);
      } else if (child_tag == DW_TAG_enumeration_type ||
                 child_tag == DW_TAG_label ||
                 child_tag == DW_TAG_lexical_block ||
                 child_tag == DW_TAG_structure_type ||
                 child_tag == DW_TAG_class_type ||
                 child_tag == DW_TAG_union_type ||
                 child_tag == DW_TAG_typedef ||
                 child_tag == DW_TAG_inlined_subroutine ||
                 child_tag == DW_TAG_variable ||
                 child_tag == DW_TAG_call_site ||
                 child_tag == DW_TAG_GNU_call_site) {
        Process(child);
      } else {
        Die() << "Unexpected tag for child of function: " << child_tag << ", "
              << EntryToString(child);
      }
    }
    auto id = AddProcessedNode<Function>(entry, return_type_id, parameters);

    if (entry.GetFlag(DW_AT_external)) {
      auto address = entry.MaybeGetAddress(DW_AT_low_pc);
      if (address) {
        // Only external functions with address are useful for ABI monitoring
        result_.symbols.push_back(Types::Symbol{
            .name = GetNameOrEmpty(entry),
            .linkage_name = entry.MaybeGetString(DW_AT_linkage_name),
            .address = *address,
            .id = id});
      }
    }
  }

  // Allocate or get already allocated STG Id for Entry.
  Id GetIdForEntry(Entry& entry) {
    const auto offset = entry.GetOffset();
    const auto [it, emplaced] = id_map_.emplace(offset, Id(-1));
    if (emplaced) {
      it->second = graph_.Allocate();
    }
    return it->second;
  }

  // Same as GetIdForEntry, but returns "void_id_" for "unspecified" references,
  // because it is normal for DWARF (5.2 Unspecified Type Entries).
  Id GetIdForReferredType(std::optional<Entry> referred_type) {
    return referred_type ? GetIdForEntry(*referred_type) : void_id_;
  }

  // Populate Id from method above with processed Node.
  template <typename Node, typename... Args>
  Id AddProcessedNode(Entry& entry, Args&&... args) {
    auto id = GetIdForEntry(entry);
    graph_.Set<Node>(id, std::forward<Args>(args)...);
    result_.all_ids.push_back(id);
    return id;
  }

  Graph& graph_;
  Id void_id_;
  Id variadic_id_;
  bool is_little_endian_binary_;
  Types& result_;
  std::unordered_map<Dwarf_Off, Id> id_map_;
};

Types ProcessEntries(std::vector<Entry> entries, bool is_little_endian_binary,
                     Graph& graph) {
  Types result;
  Id void_id = graph.Add<Void>();
  Id variadic_id = graph.Add<Variadic>();
  Processor processor(graph, void_id, variadic_id, is_little_endian_binary,
                      result);
  for (auto& entry : entries) {
    processor.Process(entry);
  }
  Check(processor.GetUnresolvedIds().empty()) << "unresolved ids";

  return result;
}

Types Process(Handler& dwarf, bool is_little_endian_binary, Graph& graph) {
  return ProcessEntries(dwarf.GetCompilationUnits(), is_little_endian_binary,
                        graph);
}

}  // namespace dwarf
}  // namespace stg
