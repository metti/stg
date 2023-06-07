// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// -*- mode: C++ -*-
//
// Copyright 2020-2022 Google LLC
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
// Author: Giuliano Procida

#include "reporting.h"

#include <cstddef>
#include <deque>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "error.h"
#include "post_processing.h"

namespace stg {

bool PrintComparison(Reporting& reporting, const Comparison& comparison,
                     std::ostream& os) {
  const auto id1 = comparison.first;
  const auto id2 = comparison.second;
  const auto* node1 = id1 ? &reporting.graph.Get(*id1) : nullptr;
  const auto* node2 = id2 ? &reporting.graph.Get(*id2) : nullptr;
  if (!id2) {
    os << node1->GetKindDescription() << " '"
       << GetDescription(reporting.graph, reporting.names, *id1)
       << "'"
       << node1->ExtraDescription()
       << " was removed\n";
    return true;
  }
  if (!id1) {
    os << node2->GetKindDescription() << " '"
       << GetDescription(reporting.graph, reporting.names, *id2)
       << "'"
       << node2->ExtraDescription()
       << " was added\n";
    return true;
  }

  const auto description1 =
      GetResolvedDescription(reporting.graph, reporting.names, *id1);
  const auto description2 =
      GetResolvedDescription(reporting.graph, reporting.names, *id2);
  os << node1->GetKindDescription() << ' ';
  if (description1 == description2)
    os << description1 << " changed";
  else
    os << "changed from " << description1 << " to " << description2;
  return false;
}

static constexpr size_t INDENT_INCREMENT = 2;

// unvisited (absent) -> started (false) -> finished (true)
using Seen = std::unordered_map<Comparison, bool, HashComparison>;

void Print(Reporting& reporting, const std::vector<DiffDetail>& details,
           Seen& seen, std::ostream& os, size_t indent);

void Print(Reporting& reporting, const Comparison& comparison, Seen& seen,
           std::ostream& os, size_t indent) {
  if (PrintComparison(reporting, comparison, os))
    return;

  const auto it = reporting.outcomes.find(comparison);
  Check(it != reporting.outcomes.end()) << "internal error: missing comparison";
  const auto& diff = it->second;

  const bool holds_changes = diff.holds_changes;
  std::pair<Seen::iterator, bool> insertion;

  if (holds_changes)
    insertion = seen.insert({comparison, false});

  if (holds_changes && !insertion.second) {
    if (!insertion.first->second)
      os << " (being reported)\n";
    else if (!diff.details.empty())
      os << " (already reported)\n";
    return;
  }

  os << '\n';
  Print(reporting, diff.details, seen, os, indent + INDENT_INCREMENT);

  if (holds_changes)
    insertion.first->second = true;
}

void Print(Reporting& reporting, const std::vector<DiffDetail>& details,
           Seen& seen, std::ostream& os, size_t indent) {
  for (const auto& detail : details) {
    os << std::string(indent, ' ') << detail.text_;
    if (!detail.edge_) {
      os << '\n';
    } else {
      if (!detail.text_.empty())
        os << ' ';
      Print(reporting, *detail.edge_, seen, os, indent);
    }
    // paragraph spacing
    if (!indent)
      os << '\n';
  }
}

void ReportPlain(Reporting& reporting, const Comparison& comparison,
                 std::ostream& output) {
  // unpack then print - want symbol diff forest rather than symbols diff tree
  const auto& diff = reporting.outcomes.at(comparison);
  Seen seen;
  Print(reporting, diff.details, seen, output, 0);
}

// Print the subtree of a diff graph starting at a given node and stopping at
// nodes that can themselves hold diffs, queuing such nodes for subsequent
// printing. Optionally, avoid printing "uninteresting" nodes - those that have
// no diff and no path to a diff that does not pass through a node that can hold
// diffs. Return whether the diff node's tree was intrinisically interesting.
bool FlatPrint(Reporting& reporting, const Comparison& comparison,
               std::unordered_set<Comparison, HashComparison>& seen,
               std::deque<Comparison>& todo, bool full, bool stop,
               std::ostream& os, size_t indent) {
  // Nodes that represent additions or removal are always interesting and no
  // recursion is possible.
  if (PrintComparison(reporting, comparison, os))
    return true;

  // Look up the diff (including node and edge changes).
  const auto it = reporting.outcomes.find(comparison);
  Check(it != reporting.outcomes.end()) << "internal error: missing comparison";
  const auto& diff = it->second;

  os << '\n';

  // Check the stopping condition.
  if (diff.holds_changes && stop) {
    // If it's a new diff-holding node, queue it.
    if (seen.insert(comparison).second)
      todo.push_back(comparison);
    return false;
  }
  // The stop flag can only be false on a non-recursive call which should be for
  // a diff-holding node.
  if (!diff.holds_changes && !stop)
    Die() << "internal error: FlatPrint called on inappropriate node";

  // Indent before describing diff details.
  indent += INDENT_INCREMENT;
  bool interesting = diff.has_changes;
  for (const auto& detail : diff.details) {
    if (!detail.edge_) {
      os << std::string(indent, ' ') << detail.text_ << '\n';
      // Node changes may not be interesting, if we allow non-change diff
      // details at some point. Just trust the has_changes flag.
    } else {
      // Edge changes are interesting if the target diff node is.
      std::ostringstream sub_os;
      sub_os << std::string(indent, ' ') << detail.text_;
      if (!detail.text_.empty())
        sub_os << ' ';
      // Set the stop flag to prevent recursion past diff-holding nodes.
      bool sub_interesting = FlatPrint(reporting, *detail.edge_, seen, todo,
                                       full, true, sub_os, indent);
      // If the sub-tree was interesting, add it.
      if (sub_interesting || full)
        os << sub_os.str();
      interesting |= sub_interesting;
    }
  }
  return interesting;
}

void ReportFlat(Reporting& reporting, const Comparison& comparison, bool full,
                std::ostream& output) {
  // We want a symbol diff forest rather than a symbol table diff tree, so
  // unpack the symbol table and then print the symbols specially.
  const auto& diff = reporting.outcomes.at(comparison);
  std::unordered_set<Comparison, HashComparison> seen;
  std::deque<Comparison> todo;
  for (const auto& detail : diff.details) {
    std::ostringstream os;
    const bool interesting =
        FlatPrint(reporting, *detail.edge_, seen, todo, full, true, os, 0);
    if (interesting || full)
      output << os.str() << '\n';
  }
  while (!todo.empty()) {
    auto comp = todo.front();
    todo.pop_front();
    std::ostringstream os;
    const bool interesting =
        FlatPrint(reporting, comp, seen, todo, full, false, os, 0);
    if (interesting || full)
      output << os.str() << '\n';
  }
}

size_t VizId(std::unordered_map<Comparison, size_t, HashComparison>& ids,
             const Comparison& comparison) {
  return ids.insert({comparison, ids.size()}).first->second;
}

void VizPrint(Reporting& reporting, const Comparison& comparison,
              std::unordered_set<Comparison, HashComparison>& seen,
              std::unordered_map<Comparison, size_t, HashComparison>& ids,
              std::ostream& os) {
  if (!seen.insert(comparison).second)
    return;

  const auto node = VizId(ids, comparison);

  const auto id1 = comparison.first;
  const auto id2 = comparison.second;
  if (!id2) {
    os << "  \"" << node << "\" [color=red, label=\"" << "removed("
       << GetDescription(reporting.graph, reporting.names, *id1)
       << reporting.graph.Get(*id1).ExtraDescription()
       << ")\"]\n";
    return;
  }
  if (!id1) {
    os << "  \"" << node << "\" [color=red, label=\"" << "added("
       << GetDescription(reporting.graph, reporting.names, *id2)
       << reporting.graph.Get(*id2).ExtraDescription()
       << ")\"]\n";
    return;
  }

  const auto it = reporting.outcomes.find(comparison);
  Check(it != reporting.outcomes.end()) << "internal error: missing comparison";
  const auto& diff = it->second;
  const char* colour = diff.has_changes ? "color=red, " : "";
  const char* shape = diff.holds_changes ? "shape=rectangle, " : "";
  const auto description1 =
      GetResolvedDescription(reporting.graph, reporting.names, *id1);
  const auto description2 =
      GetResolvedDescription(reporting.graph, reporting.names, *id2);
  if (description1 == description2)
    os << "  \"" << node << "\" [" << colour << shape << "label=\""
       << description1 << "\"]\n";
  else
    os << "  \"" << node << "\" [" << colour << shape << "label=\""
       << description1 << " -> " << description2 << "\"]\n";

  size_t index = 0;
  for (const auto& detail : diff.details) {
    if (!detail.edge_) {
      // attribute change, create an implicit edge and node
      os << "  \"" << node << "\" -> \"" << node << ':' << index << "\"\n"
         << "  \"" << node << ':' << index << "\" [color=red, label=\""
         << detail.text_ << "\"]\n";
      ++index;
    } else {
      const auto& to = *detail.edge_;
      VizPrint(reporting, to, seen, ids, os);
      os << "  \"" << node << "\" -> \"" << VizId(ids, to) << "\" [label=\""
         << detail.text_ << "\"]\n";
    }
  }
}


void ReportViz(Reporting& reporting, const Comparison& comparison,
               std::ostream& output) {
  output << "digraph \"ABI diff\" {\n";
  std::unordered_set<Comparison, HashComparison> seen;
  std::unordered_map<Comparison, size_t, HashComparison> ids;
  VizPrint(reporting, comparison, seen, ids, output);
  output << "}\n";
}

void Report(Reporting& reporting, const Comparison& comparison,
            std::ostream& output) {
  switch (reporting.options.format) {
    case OutputFormat::PLAIN: {
      ReportPlain(reporting, comparison, output);
      break;
    }
    case OutputFormat::FLAT:
    case OutputFormat::SMALL: {
      bool full = reporting.options.format == OutputFormat::FLAT;
      ReportFlat(reporting, comparison, full, output);
      break;
    }
    case OutputFormat::SHORT: {
      std::stringstream report;
      ReportFlat(reporting, comparison, false, report);
      std::vector<std::string> report_lines;
      std::string line;
      while (std::getline(report, line))
        report_lines.push_back(line);
      report_lines = stg::PostProcess(report_lines,
                                      reporting.options.max_crc_only_changes);
      for (const auto& line : report_lines) {
        output << line << '\n';
      }
      break;
    }
    case OutputFormat::VIZ: {
      ReportViz(reporting, comparison, output);
      break;
    }
  }
}

}  // namespace stg
