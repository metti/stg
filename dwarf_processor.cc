#include "dwarf_processor.h"

#include <dwarf.h>

#include <unordered_map>
#include <utility>
#include <vector>

#include "dwarf.h"
#include "error.h"
#include "stg.h"

namespace stg {
namespace dwarf {

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
