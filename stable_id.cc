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
// Author: Siddharth Nayyar

#include "stable_id.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "graph.h"
#include "hashing.h"

namespace stg {

namespace {

// This combines 2 hash values while decaying (by shifting right) the second
// value. This prevents the most significant bits of the first hash from being
// affected by the decayed hash. Hash combination is done using a simple XOR
// operation to preserve the separation of higher and lower bits. Note that XOR
// is not a very effective method of mixing hash values if the values are
// generated with a weak hashing algorithm.
template <uint8_t decay>
constexpr HashValue DecayHashCombine(HashValue a, HashValue b) {
  static_assert(decay > 0 && decay < 32, "decay must lie inside (0, 32)");
  return a ^ (b >> decay);
}

// Decaying hashes are combined in reverse since the each successive hashable
// should be decayed 1 more time than the previous hashable and the last
// hashable should receieve the most decay.
template <uint8_t decay, typename Type, typename Hash>
HashValue DecayHashCombineInReverse(const std::vector<Type>& hashables,
                                    Hash& hash) {
  HashValue result(0);
  for (auto it = hashables.crbegin(); it != hashables.crend(); ++it) {
    result = DecayHashCombine<decay>(hash(*it), result);
  }
  return result;
}

}  // namespace

HashValue StableId::operator()(Id id) {
  auto [it, inserted] = stable_id_cache_.emplace(id, 0);
  if (inserted) {
    it->second = graph_.Apply<HashValue>(*this, id);
  }
  return it->second;
}

HashValue StableId::operator()(const Void&) {
  return hash_("void");
}

HashValue StableId::operator()(const Variadic&) {
  return hash_("variadic");
}

HashValue StableId::operator()(const PointerReference& x) {
  return DecayHashCombine<2>(hash_('r', static_cast<uint32_t>(x.kind)),
                             (*this)(x.pointee_type_id));
}

HashValue StableId::operator()(const Typedef& x) {
  return hash_('t', x.name);
}

HashValue StableId::operator()(const Qualified& x) {
  return DecayHashCombine<2>(hash_('q', static_cast<uint32_t>(x.qualifier)),
                             (*this)(x.qualified_type_id));
}

HashValue StableId::operator()(const Primitive& x) {
  return hash_('p', x.name);
}

HashValue StableId::operator()(const Array& x) {
  return DecayHashCombine<2>(hash_('a', x.number_of_elements),
                             (*this)(x.element_type_id));
}

HashValue StableId::operator()(const BaseClass& x) {
  return DecayHashCombine<2>(hash_('b', static_cast<uint32_t>(x.inheritance)),
                             (*this)(x.type_id));
}

HashValue StableId::operator()(const Method& x) {
  return hash_(x.mangled_name, static_cast<uint32_t>(x.kind));
}

HashValue StableId::operator()(const Member& x) {
  HashValue hash = hash_('m', x.name, x.bitsize);
  hash = DecayHashCombine<20>(hash, hash_(x.offset));
  if (x.name.empty()) {
    return DecayHashCombine<2>(hash, (*this)(x.type_id));
  } else {
    return DecayHashCombine<8>(hash, (*this)(x.type_id));
  }
}

HashValue StableId::operator()(const StructUnion& x) {
  HashValue hash = hash_('S', static_cast<uint32_t>(x.kind), x.name,
                         static_cast<bool>(x.definition));
  if (!x.name.empty() || !x.definition) {
    return hash;
  }

  return DecayHashCombine<2>(
      hash, DecayHashCombineInReverse<8>(x.definition->methods, *this) ^
                DecayHashCombineInReverse<8>(x.definition->members, *this));
}

HashValue StableId::operator()(const Enumeration& x) {
  HashValue hash = hash_('e', x.name, static_cast<bool>(x.definition));
  if (!x.name.empty() || !x.definition) {
    return hash;
  }

  auto hash_enum = [this](const std::pair<std::string, int64_t>& e) {
    return hash_(e.first, e.second);
  };
  return DecayHashCombine<2>(
      hash, DecayHashCombineInReverse<8>(x.definition->enumerators, hash_enum));
}

HashValue StableId::operator()(const Function& x) {
  return DecayHashCombine<2>(hash_('f', (*this)(x.return_type_id)),
                             DecayHashCombineInReverse<4>(x.parameters, *this));
}

HashValue StableId::operator()(const ElfSymbol& x) {
  HashValue hash = hash_('s', x.symbol_name);
  if (x.version_info) {
    hash = DecayHashCombine<16>(
        hash, hash_(x.version_info->name, x.version_info->is_default));
  }
  return hash;
}

HashValue StableId::operator()(const Symbols&) {
  return hash_("symtab");
}

}  // namespace stg
