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

}  // namespace

// Transforms DWARF entries to STG.
class Processor {
 public:
  Processor(Graph& graph, Id void_id, Types& result)
      : graph_(graph), void_id_(void_id), result_(result) {}

  void Process(Entry& entry) {
    ++result_.processed_entries;
    auto tag = entry.GetTag();
    switch (tag) {
      case DW_TAG_class_type:
        ProcessStructUnion(entry, StructUnion::Kind::CLASS);
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
    auto type_name = GetName(entry);
    size_t bit_size = GetBitSize(entry);
    // Round up bit_size / 8 to get minimal needed storage size in bytes.
    size_t byte_size = (bit_size + 7) / 8;
    AddProcessedNode<Primitive>(entry, type_name, GetEncoding(entry), bit_size,
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
    // TODO: support offset and bitsize from DWARF
    AddProcessedNode<Member>(entry, std::move(name), referred_type_id,
                             /* offset = */ 0,
                             /* bitsize = */ 0);
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
  Types& result_;
  std::unordered_map<Dwarf_Off, Id> id_map_;
};

Types ProcessEntries(std::vector<Entry> entries, Graph& graph) {
  Types result;
  Id void_id = graph.Add<Void>();
  Processor processor(graph, void_id, result);
  for (auto& entry : entries) {
    processor.Process(entry);
  }
  for (const auto& id : processor.GetUnresolvedIds()) {
    // TODO: replace with "Die"
    graph.Set<Variadic>(id);
  }

  return result;
}

Types Process(Handler& dwarf, Graph& graph) {
  return ProcessEntries(dwarf.GetCompilationUnits(), graph);
}

}  // namespace dwarf
}  // namespace stg
