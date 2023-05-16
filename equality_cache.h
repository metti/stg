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

#ifndef STG_EQUALITY_CACHE_H_
#define STG_EQUALITY_CACHE_H_

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "equality.h"
#include "graph.h"
#include "hashing.h"
#include "metrics.h"

namespace stg {

// Equality cache - for use with the Equals function object
//
// This supports many features, some of probably limited long-term utility.
//
// It caches equalities (symmetrically) using union-find with path halving and
// union by rank.
//
// It caches inequalities (symmetrically); the inequalities are updated as part
// of the union operation.
//
// Node hashes such as those generated by the Fingerprint function object may be
// supplied to avoid equality testing when hashes differ.
struct EqualityCache {
  EqualityCache(const std::unordered_map<Id, HashValue>& hashes,
                Metrics& metrics)
      : hashes(hashes),
        query_count(metrics, "cache.query_count"),
        query_equal_ids(metrics, "cache.query_equal_ids"),
        query_unequal_hashes(metrics, "cache.query_unequal_hashes"),
        query_equal_representatives(metrics,
                                    "cache.query_equal_representatives"),
        query_inequality_found(metrics, "cache.query_inequality_found"),
        query_not_found(metrics, "cache.query_not_found"),
        find_halved(metrics, "cache.find_halved"),
        union_known(metrics, "cache.union_known"),
        union_rank_swap(metrics, "cache.union_rank_swap"),
        union_rank_increase(metrics, "cache.union_rank_increase"),
        union_rank_zero(metrics, "cache.union_rank_zero"),
        union_unknown(metrics, "cache.union_unknown"),
        disunion_known_hash(metrics, "cache.disunion_known_hash"),
        disunion_known_inequality(metrics, "cache.disunion_known_inequality"),
        disunion_unknown(metrics, "cache.disunion_unknown") {}

  std::optional<bool> Query(const Pair& comparison) {
    ++query_count;
    const auto& [id1, id2] = comparison;
    if (id1 == id2) {
      ++query_equal_ids;
      return std::make_optional(true);
    }
    if (DistinctHashes(id1, id2)) {
      ++query_unequal_hashes;
      return std::make_optional(false);
    }
    const Id fid1 = Find(id1);
    const Id fid2 = Find(id2);
    if (fid1 == fid2) {
      ++query_equal_representatives;
      return std::make_optional(true);
    }
    auto not_it = inequalities.find(fid1);
    if (not_it != inequalities.end()) {
      auto not_it2 = not_it->second.find(fid2);
      if (not_it2 != not_it->second.end()) {
        ++query_inequality_found;
        return std::make_optional(false);
      }
    }
    ++query_not_found;
    return std::nullopt;
  }

  void AllSame(const std::vector<Pair>& comparisons) {
    for (const auto& [id1, id2] : comparisons) {
      Union(id1, id2);
    }
  }

  void AllDifferent(const std::vector<Pair>& comparisons) {
    for (const auto& [id1, id2] : comparisons) {
      Disunion(id1, id2);
    }
  }

  bool DistinctHashes(Id id1, Id id2) {
    const auto it1 = hashes.find(id1);
    const auto it2 = hashes.find(id2);
    return it1 != hashes.end() && it2 != hashes.end()
        && it1->second != it2->second;
  }

  Id Find(Id id) {
    // path halving
    while (true) {
      auto it = mapping.find(id);
      if (it == mapping.end()) {
        return id;
      }
      auto& parent = it->second;
      auto parent_it = mapping.find(parent);
      if (parent_it == mapping.end()) {
        return parent;
      }
      auto parent_parent = parent_it->second;
      id = parent = parent_parent;
      ++find_halved;
    }
  }

  size_t GetRank(Id id) {
    auto it = rank.find(id);
    return it == rank.end() ? 0 : it->second;
  }

  void SetRank(Id id, size_t r) {
    if (r) {
      rank[id] = r;
    } else {
      rank.erase(id);
    }
  }

  void Union(Id id1, Id id2) {
    Check(!DistinctHashes(id1, id2)) << "union with distinct hashes";
    Id fid1 = Find(id1);
    Id fid2 = Find(id2);
    if (fid1 == fid2) {
      ++union_known;
      return;
    }
    size_t rank1 = GetRank(fid1);
    size_t rank2 = GetRank(fid2);
    if (rank1 > rank2) {
      std::swap(fid1, fid2);
      std::swap(rank1, rank2);
      ++union_rank_swap;
    }
    // rank1 <= rank2
    if (rank1 == rank2) {
      SetRank(fid2, rank2 + 1);
      ++union_rank_increase;
    }
    if (rank1) {
      SetRank(fid1, 0);
      ++union_rank_zero;
    }
    mapping.insert({fid1, fid2});
    ++union_unknown;

    // move inequalities from fid1 to fid2
    auto not_it = inequalities.find(fid1);
    if (not_it != inequalities.end()) {
      auto& source = not_it->second;
      auto& target = inequalities[fid2];
      for (auto fid : source) {
        Check(fid != fid2) << "union of unequal";
        target.insert(fid);
        auto& target2 = inequalities[fid];
        target2.erase(fid1);
        target2.insert(fid2);
      }
    }
  }

  void Disunion(Id id1, Id id2) {
    if (DistinctHashes(id1, id2)) {
      ++disunion_known_hash;
      return;
    }
    const Id fid1 = Find(id1);
    const Id fid2 = Find(id2);
    Check(fid1 != fid2) << "disunion of equal";
    if (inequalities[fid1].insert(fid2).second) {
      inequalities[fid2].insert(fid1);
      ++disunion_unknown;
    } else {
      ++disunion_known_inequality;
    }
  }

  const std::unordered_map<Id, HashValue>& hashes;
  std::unordered_map<Id, Id> mapping;
  std::unordered_map<Id, size_t> rank;
  std::unordered_map<Id, std::unordered_set<Id>> inequalities;

  Counter query_count;
  Counter query_equal_ids;
  Counter query_unequal_hashes;
  Counter query_equal_representatives;
  Counter query_inequality_found;
  Counter query_not_found;
  Counter find_halved;
  Counter union_known;
  Counter union_rank_swap;
  Counter union_rank_increase;
  Counter union_rank_zero;
  Counter union_unknown;
  Counter disunion_known_hash;
  Counter disunion_known_inequality;
  Counter disunion_unknown;
};

struct SimpleEqualityCache {
  explicit SimpleEqualityCache(Metrics& metrics)
      : query_count(metrics, "simple_cache.query_count"),
        query_equal_ids(metrics, "simple_cache.query_equal_ids"),
        query_known_equality(metrics, "simple_cache.query_known_equality"),
        known_equality_inserts(metrics, "simple_cache.known_equality_inserts") {
  }

  std::optional<bool> Query(const Pair& comparison) {
    ++query_count;
    const auto& [id1, id2] = comparison;
    if (id1 == id2) {
      ++query_equal_ids;
      return {true};
    }
    if (known_equalities.count(comparison)) {
      ++query_known_equality;
      return {true};
    }
    return std::nullopt;
  }

  void AllSame(const std::vector<Pair>& comparisons) {
    for (const auto& comparison : comparisons) {
      ++known_equality_inserts;
      known_equalities.insert(comparison);
    }
  }

  void AllDifferent(const std::vector<Pair>&) {}

  std::unordered_set<Pair> known_equalities;

  Counter query_count;
  Counter query_equal_ids;
  Counter query_known_equality;
  Counter known_equality_inserts;
};

}  // namespace stg

#endif  // STG_EQUALITY_CACHE_H_
