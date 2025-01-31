////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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

#include "boost_sort.hpp"

namespace {

using namespace irs;

sort::ptr make_json(std::string_view /*args*/) {
  return std::make_unique<boost_sort>();
}

struct volatile_boost_score_ctx : score_ctx {
  volatile_boost_score_ctx(const filter_boost* volatile_boost,
                           score_t boost) noexcept
    : boost{boost}, volatile_boost{volatile_boost} {
    assert(volatile_boost);
  }

  score_t boost;
  const filter_boost* volatile_boost;
};

struct prepared final : PreparedSortBase<void> {
  IndexFeatures features() const noexcept override {
    return IndexFeatures::NONE;
  }

  ScoreFunction prepare_scorer(const sub_reader&, const term_reader&,
                               const byte_type*,
                               const irs::attribute_provider& attrs,
                               irs::score_t boost) const override {
    auto* volatile_boost = irs::get<irs::filter_boost>(attrs);

    if (!volatile_boost) {
      return ScoreFunction::Constant(boost);
    }

    return {
      std::make_unique<volatile_boost_score_ctx>(volatile_boost, boost),
      [](irs::score_ctx* ctx, irs::score_t* res) noexcept {
        auto& state = *reinterpret_cast<volatile_boost_score_ctx*>(ctx);
        *res = state.volatile_boost->value * state.boost;
      }};
  }
};

}  // namespace

namespace iresearch {

/*static*/ void boost_sort::init() {
  REGISTER_SCORER_JSON(boost_sort, make_json);
}

boost_sort::boost_sort() noexcept : sort(irs::type<boost_sort>::get()) {}

sort::prepared::ptr boost_sort::prepare() const {
  // FIXME can avoid allocation
  return std::make_unique<::prepared>();
}

}  // namespace iresearch
