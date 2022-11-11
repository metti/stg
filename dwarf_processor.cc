#include "dwarf_processor.h"

#include <vector>

#include "dwarf.h"

namespace stg {
namespace dwarf {

// Transforms DWARF entries to STG.
class Processor {
 public:
  Processor(Types& result) : result_(result) {}

  void Process(Entry& entry) {
    ++result_.processed_entries;
    ProcessAllChildren(entry);
  }

 private:
  void ProcessAllChildren(Entry& entry) {
    for (auto& child : entry.GetChildren()) {
      Process(child);
    }
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
