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

#pragma once

#include "analysis/token_stream.hpp"
#include "utils/type_info.hpp"

namespace iresearch::analysis {

class analyzer : public token_stream {
 public:
  using ptr = std::unique_ptr<analyzer>;

  explicit analyzer(const type_info& type) noexcept;

  virtual bool reset(std::string_view data) = 0;

  constexpr type_info::type_id type() const noexcept { return type_; }

 private:
  type_info::type_id type_;
};

class empty_analyzer final : public analyzer {
 public:
  static constexpr std::string_view type_name() noexcept {
    return "empty_analyzer";
  }

  empty_analyzer() noexcept;

  virtual attribute* get_mutable(irs::type_info::type_id) override {
    return nullptr;
  }

  virtual bool next() override { return false; }

  virtual bool reset(std::string_view) override { return false; }
};

}  // namespace iresearch::analysis
