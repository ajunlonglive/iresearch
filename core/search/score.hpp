////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#ifndef IRESEARCH_SCORE_H
#define IRESEARCH_SCORE_H

#include "sort.hpp"
#include "utils/attributes.hpp"

namespace iresearch {

// Represents a score related for the particular document
struct score : attribute, ScoreFunction {
  static const score kNoScore;

  static constexpr std::string_view type_name() noexcept {
    return "iresearch::score";
  }

  template<typename Provider>
  static const score& get(const Provider& attrs) {
    const auto* score = irs::get<irs::score>(attrs);
    return score ? *score : kNoScore;
  }

  using ScoreFunction::operator=;
};

using Scorers = SmallVector<ScoreFunction, 2>;

// Prepare scorer for each of the bucket.
Scorers PrepareScorers(std::span<const OrderBucket> buckets,
                       const sub_reader& segment, const term_reader& field,
                       const byte_type* stats, const attribute_provider& doc,
                       score_t boost);

// Compiles a set of prepared scorers into a single score function.
ScoreFunction CompileScorers(Scorers&& scorers);

template<typename... Args>
ScoreFunction CompileScore(Args&&... args) {
  return CompileScorers(PrepareScorers(std::forward<Args>(args)...));
}

// Prepare empty collectors, i.e. call collect(...) on each of the
// buckets without explicitly collecting field or term statistics,
// e.g. for 'all' filter.
void PrepareCollectors(std::span<const OrderBucket> order, byte_type* stats,
                       const index_reader& index);

}  // namespace iresearch

#endif  // IRESEARCH_SCORE_H
