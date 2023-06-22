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
// Author: Giuliano Procida

#ifndef STG_UNIFICATION_H_
#define STG_UNIFICATION_H_

#include <unordered_map>
#include <unordered_set>

#include "graph.h"
#include "metrics.h"

namespace stg {

// Keep track of which nodes have been substituted.
class UnificationCache {
 public:
  UnificationCache(const Graph& graph, Metrics& metrics)
      : mapping_(graph.MakeDenseIdMapping()),
        find_query_(metrics, "cache.find_query"),
        find_halved_(metrics, "cache.find_halved"),
        union_known_(metrics, "cache.union_known"),
        union_unknown_(metrics, "cache.union_unknown") {}

  Id Find(Id id) {
    ++find_query_;
    // path halving - tiny performance gain
    while (true) {
      // note: safe to take references as mapping cannot grow after this
      auto& parent = mapping_[id];
      if (parent == id) {
        return id;
      }
      auto& parent_parent = mapping_[parent];
      if (parent_parent == parent) {
        return parent;
      }
      id = parent = parent_parent;
      ++find_halved_;
    }
  }

  void Union(Id id1, Id id2) {
    // id2 will always be preferred as a parent node; interpreted as a
    // substitution, id1 will be replaced by id2
    const Id fid1 = Find(id1);
    const Id fid2 = Find(id2);
    if (fid1 == fid2) {
      ++union_known_;
      return;
    }
    mapping_[fid1] = fid2;
    ++union_unknown_;
  }

  // update id to representative id
  void Update(Id& id) {
    const Id fid = Find(id);
    // avoid silent stores
    if (fid != id) {
      id = fid;
    }
  }

 private:
  Graph::DenseIdMapping mapping_;
  Counter find_query_;
  Counter find_halved_;
  Counter union_known_;
  Counter union_unknown_;
};

bool Unify(const Graph& graph, UnificationCache& cache, Id id1, Id id2);

}  // namespace stg

#endif  // STG_UNIFICATION_H_
