#include "dwarf_processor.h"

#include <dwarf.h>

#include <vector>

#include "dwarf.h"
#include "error.h"

namespace stg {
namespace dwarf {

// Transforms DWARF entries to STG.
class Processor {
 public:
  Processor(Types& result) : result_(result) {}

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

  Types& result_;
};

Types ProcessEntries(std::vector<Entry> entries) {
  Types result;
  Processor processor(result);
  for (auto& entry : entries) {
    processor.Process(entry);
  }
  return result;
}

Types Process(Handler& dwarf) {
  return ProcessEntries(dwarf.GetCompilationUnits());
}

}  // namespace dwarf
}  // namespace stg
