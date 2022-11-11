#ifndef STG_DWARF_PROCESSOR_H_
#define STG_DWARF_PROCESSOR_H_

#include <vector>

#include "dwarf.h"

namespace stg {
namespace dwarf {

struct Types {
  size_t processed_entries = 0;
};

// Process every compilation unit from DWARF and returns processed STG along
// with information needed for matching to ELF symbols.
Types Process(Handler& dwarf);

// Process every entry passed there. It should be used only for testing.
Types ProcessEntries(std::vector<Entry> entries);

}  // namespace dwarf
}  // namespace stg

#endif  // STG_DWARF_PROCESSOR_H_
