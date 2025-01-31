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

#ifndef IRESEARCH_TERM_FILTER_H
#define IRESEARCH_TERM_FILTER_H

#include "search/filter.hpp"
#include "utils/string.hpp"

namespace iresearch {

class by_term;
struct filter_visitor;

// Options for term filter
struct by_term_options {
  using filter_type = by_term;

  bstring term;

  bool operator==(const by_term_options& rhs) const noexcept {
    return term == rhs.term;
  }

  size_t hash() const noexcept { return hash_utils::Hash(term); }
};

// User-side term filter
class by_term : public filter_base<by_term_options> {
 public:
  static prepared::ptr prepare(const index_reader& rdr, const Order& ord,
                               score_t boost, std::string_view field, bytes_view term);

  static void visit(const sub_reader& segment, const term_reader& field,
                    bytes_view term, filter_visitor& visitor);

  using filter::prepare;

  virtual prepared::ptr prepare(
    const index_reader& rdr, const Order& ord, score_t boost,
    const attribute_provider* /*ctx*/) const override {
    return prepare(rdr, ord, boost * this->boost(), field(), options().term);
  }
};

}  // namespace iresearch

namespace std {

template<>
struct hash<::iresearch::by_term_options> {
  size_t operator()(const ::iresearch::by_term_options& v) const noexcept {
    return v.hash();
  }
};

}  // namespace std

#endif  // IRESEARCH_TERM_FILTER_H
