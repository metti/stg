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
// Author: Giuliano Procida

#include "deduplication.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "equality.h"
#include "equality_cache.h"
#include "substitution.h"

namespace stg {

Id Deduplicate(Graph& graph, Id root, const Hashes& hashes, Metrics& metrics) {
  // Partition the nodes by hash.
  std::unordered_map<HashValue, std::vector<Id>> partitions;
  {
    Time x(metrics, "partition nodes");
    for (const auto& [id, fp] : hashes) {
      partitions[fp].push_back(id);
    }
  }
  Counter(metrics, "deduplicate.nodes") = hashes.size();
  Counter(metrics, "deduplicate.hashes") = partitions.size();

  Histogram hash_partition_size(metrics, "deduplicate.hash_partition_size");
  Counter min_comparisons(metrics, "deduplicate.min_comparisons");
  Counter max_comparisons(metrics, "deduplicate.max_comparisons");
  for (const auto& [fp, ids] : partitions) {
    const auto n = ids.size();
    hash_partition_size.Add(n);
    min_comparisons += n - 1;
    max_comparisons += n * (n - 1) / 2;
  }

  // Refine partitions of nodes with the same fingerprints.
  EqualityCache cache(hashes, metrics);
  Equals<EqualityCache> equals(graph, cache);
  Counter equalities(metrics, "deduplicate.equalities");
  Counter inequalities(metrics, "deduplicate.inequalities");
  {
    Time x(metrics, "find duplicates");
    for (auto& [fp, ids] : partitions) {
      while (ids.size() > 1) {
        std::vector<Id> todo;
        Id candidate = ids[0];
        for (size_t i = 1; i < ids.size(); ++i) {
          if (equals(ids[i], candidate)) {
            ++equalities;
          } else {
            todo.push_back(ids[i]);
            ++inequalities;
          }
        }
        std::swap(todo, ids);
      }
    }
  }

  // Keep one representative of each set of duplicates.
  Counter unique(metrics, "deduplicate.unique");
  Counter duplicate(metrics, "deduplicate.duplicate");
  auto remap = [&cache](Id& id) {
    // update id to representative id, avoiding silent stores
    Id fid = cache.Find(id);
    if (fid != id) {
      id = fid;
    }
  };
  Substitute<decltype(remap)> substitute(graph, remap);
  {
    Time x(metrics, "rewrite");
    for (const auto& [id, fp] : hashes) {
      Id fid = cache.Find(id);
      if (fid != id) {
        graph.Remove(id);
        ++duplicate;
      } else {
        substitute(id);
        ++unique;
      }
    }
  }

  // In case the root node was remapped.
  substitute.Update(root);
  return root;
}

}  // namespace stg
