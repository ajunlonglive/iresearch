////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#ifndef IRESEARCH_TEXT_TOKEN_STEMMING_STREAM_H
#define IRESEARCH_TEXT_TOKEN_STEMMING_STREAM_H

#include <unicode/locid.h>

#include "analyzers.hpp"
#include "token_attributes.hpp"
#include "utils/attribute_helper.hpp"
#include "utils/snowball_stemmer.hpp"

namespace iresearch {
namespace analysis {

////////////////////////////////////////////////////////////////////////////////
/// @class stemming_token_stream
/// @brief an analyser capable of stemming the text, treated as a single token,
///        for supported languages
/// @note expects UTF-8 encoded input
////////////////////////////////////////////////////////////////////////////////
class stemming_token_stream final : public analyzer, private util::noncopyable {
 public:
  struct options_t {
    icu::Locale locale;

    options_t() : locale{"C"} { locale.setToBogus(); }
  };

  static constexpr std::string_view type_name() noexcept { return "stem"; }
  static void init();  // for trigering registration in a static build

  explicit stemming_token_stream(const options_t& options);
  virtual attribute* get_mutable(
    irs::type_info::type_id type) noexcept override final {
    return irs::get_mutable(attrs_, type);
  }
  virtual bool next() override;
  virtual bool reset(std::string_view data) override;

 private:
  using attributes =
    std::tuple<increment, offset,
               term_attribute>;  // token value with evaluated quotes

  attributes attrs_;
  options_t options_;
  std::string buf_;
  stemmer_ptr stemmer_;
  bool term_eof_;
};

}  // namespace analysis
}  // namespace iresearch

#endif
