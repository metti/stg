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

#include "dwarf.h"
#include "error.h"
#include "stg.h"

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

size_t GetBitSize(Entry& entry) {
  if (auto byte_size = entry.MaybeGetUnsignedConstant(DW_AT_byte_size)) {
    return *byte_size * 8;
  } else if (auto bit_size = entry.MaybeGetUnsignedConstant(DW_AT_bit_size)) {
    return *bit_size;
  }
  Die() << "Bit size was not found for " << EntryToString(entry);
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

}  // namespace

// Transforms DWARF entries to STG.
class Processor {
 public:
  Processor(Graph &graph, Types& result) : graph_(graph), result_(result) {}

  void Process(Entry& entry) {
    ++result_.processed_entries;
    auto tag = entry.GetTag();
    switch (tag) {
      case DW_TAG_compile_unit:
        ProcessCompileUnit(entry);
        break;
      case DW_TAG_base_type:
        ProcessBaseType(entry);
        break;

      default:
        // TODO: die on unexpected tag, when this switch contains
        // all expected tags
        break;
    }
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

  // Allocate or get already allocated STG Id for Entry.
  Id GetIdForEntry(Entry& entry) {
    const auto offset = entry.GetOffset();
    const auto [it, emplaced] = id_map_.emplace(offset, Id(-1));
    if (emplaced) {
      it->second = graph_.Allocate();
    }
    return it->second;
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
  Types& result_;
  std::unordered_map<Dwarf_Off, Id> id_map_;
};

Types ProcessEntries(std::vector<Entry> entries, Graph& graph) {
  Types result;
  Processor processor(graph, result);
  for (auto& entry : entries) {
    processor.Process(entry);
  }
  return result;
}

Types Process(Handler& dwarf, Graph& graph) {
  return ProcessEntries(dwarf.GetCompilationUnits(), graph);
}

}  // namespace dwarf
}  // namespace stg
