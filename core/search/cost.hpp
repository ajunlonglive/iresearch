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
////////////////////////////////////////////////////////////////////////////////

#ifndef IRESEARCH_COST_H
#define IRESEARCH_COST_H

#include <functional>

#include "utils/attribute_provider.hpp"
#include "utils/attributes.hpp"

namespace iresearch {

// Represents an estimated cost of the query execution.
class cost final : public attribute {
 public:
  using cost_t = uint64_t;
  using cost_f = std::function<cost_t()>;

  static constexpr std::string_view type_name() noexcept { return "iresearch::cost"; }

  static constexpr cost_t kMax = std::numeric_limits<cost_t>::max();

  cost() = default;

  explicit cost(cost_t value) noexcept : value_(value), init_(true) {}

  explicit cost(cost_f&& func) noexcept(
    std::is_nothrow_move_constructible_v<cost_f>)
    : func_(std::move(func)), init_(false) {}

  // Returns a value of the "cost" attribute in the specified "src"
  // collection, or "def" value if there is no "cost" attribute in "src".
  template<typename Provider>
  static cost_t extract(const Provider& src, cost_t def = kMax) noexcept {
    if (auto* attr = irs::get<irs::cost>(src); attr) {
      return attr->estimate();
    } else {
      return def;
    }
  }

  // Sets the estimation value.
  void reset(cost_t value) noexcept {
    value_ = value;
    init_ = true;
  }

  // Sets the estimation rule.
  void reset(cost_f&& eval) noexcept(
    std::is_nothrow_move_assignable_v<cost_f>) {
    assert(eval);
    func_ = std::move(eval);
    init_ = false;
  }

  // Estimate the query according to the provided estimation function.
  // Return estimated cost.
  cost_t estimate() const {
    if (!init_) {
      assert(func_);
      value_ = func_();
      init_ = true;
    }
    return value_;
  }

 private:
  cost_f func_{[] { return 0; }};  // evaluation function
  mutable cost_t value_{0};
  mutable bool init_{true};
};

}  // namespace iresearch

#endif  // IRESEARCH_COST_H
