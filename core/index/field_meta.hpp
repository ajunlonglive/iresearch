////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#ifndef IRESEARCH_FIELD_META_H
#define IRESEARCH_FIELD_META_H

#include <set>

#include "index/index_features.hpp"
#include "utils/type_limits.hpp"
#include "utils/attributes.hpp"

namespace iresearch {

struct field_stats {
  // Total number of terms
  uint32_t len{};
  // Number of overlapped terms
  uint32_t num_overlap{};
  // Maximum number of terms in a field
  uint32_t max_term_freq{};
  // Number of unique terms
  uint32_t num_unique{};
};

using feature_map_t = std::map<type_info::type_id, field_id>;
using feature_set_t = std::set<type_info::type_id>;
using features_t = std::span<const type_info::type_id>;

// Represents field metadata
struct field_meta {
 public:
  static const field_meta kEmpty;

  field_meta() = default;
  field_meta(const field_meta&) = default;
  field_meta(field_meta&& rhs) noexcept;
  field_meta(std::string_view field, IndexFeatures index_features);

  field_meta& operator=(field_meta&& rhs) noexcept;
  field_meta& operator=(const field_meta&) = default;

  bool operator==(const field_meta& rhs) const;
  bool operator!=(const field_meta& rhs) const { return !(*this == rhs); }

  feature_map_t features;
  std::string name;
  IndexFeatures index_features{IndexFeatures::NONE};
};

static_assert(std::is_move_constructible<field_meta>::value,
              "default move constructor expected");

}  // namespace iresearch

#endif
