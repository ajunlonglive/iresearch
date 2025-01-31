////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2022 ArangoDB GmbH, Cologne, Germany
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

#pragma once

#include "search/filter.hpp"
#include "search/prepared_state_visitor.hpp"
#include "search/states/phrase_state.hpp"
#include "search/states_cache.hpp"

namespace iresearch {

class FixedPhraseQuery;
class VariadicPhraseQuery;

// Prepared phrase query implementation
template<typename State>
class PhraseQuery : public filter::prepared {
 public:
  using states_t = states_cache<State>;
  using positions_t = std::vector<uint32_t>;

  // Returns features required for phrase filter
  static constexpr IndexFeatures kRequiredFeatures =
    IndexFeatures::FREQ | IndexFeatures::POS;

  PhraseQuery(states_t&& states, positions_t&& positions, bstring&& stats,
              score_t boost) noexcept
    : prepared{boost},
      states_{std::move(states)},
      positions_{std::move(positions)},
      stats_{std::move(stats)} {}

  void visit(const sub_reader& segment, PreparedStateVisitor& visitor,
             score_t boost) const final {
    static_assert(std::is_same_v<State, FixedPhraseState> ||
                  std::is_same_v<State, VariadicPhraseState>);

    if (auto state = states_.find(segment); state) {
      boost *= this->boost();
      if constexpr (std::is_same_v<State, FixedPhraseState>) {
        visitor.Visit(static_cast<const FixedPhraseQuery&>(*this), *state,
                      boost);
      } else if constexpr (std::is_same_v<State, FixedPhraseState>) {
        visitor.Visit(static_cast<const VariadicPhraseQuery&>(*this), *state,
                      boost);
      }
    }
  }

  states_t states_;
  positions_t positions_;
  bstring stats_;
};

class FixedPhraseQuery final : public PhraseQuery<FixedPhraseState> {
 public:
  FixedPhraseQuery(states_t&& states, positions_t&& positions, bstring&& stats,
                   score_t boost) noexcept
    : PhraseQuery{std::move(states), std::move(positions), std::move(stats),
                  boost} {}

  using filter::prepared::execute;

  doc_iterator::ptr execute(const ExecutionContext& ctx) const override;

  doc_iterator::ptr ExecuteWithOffsets(const irs::sub_reader& segment) const;
};

class VariadicPhraseQuery final : public PhraseQuery<VariadicPhraseState> {
 public:
  VariadicPhraseQuery(states_t&& states, positions_t&& positions,
                      bstring&& stats, score_t boost) noexcept
    : PhraseQuery{std::move(states), std::move(positions), std::move(stats),
                  boost} {}

  using filter::prepared::execute;

  doc_iterator::ptr execute(const ExecutionContext& ctx) const override;

  doc_iterator::ptr ExecuteWithOffsets(const irs::sub_reader& segment) const;
};

}  // namespace iresearch
