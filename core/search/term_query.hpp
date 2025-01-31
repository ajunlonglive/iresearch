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

#ifndef IRESEARCH_TERM_QUERY_H
#define IRESEARCH_TERM_QUERY_H

#include "search/filter.hpp"
#include "search/states/term_state.hpp"
#include "search/states_cache.hpp"

namespace iresearch {

// Compiled query suitable for filters with a single term like "by_term"
class TermQuery final : public filter::prepared {
 public:
  using States = states_cache<TermState>;

  explicit TermQuery(States&& states, bstring&& stats, score_t boost);

  doc_iterator::ptr execute(const ExecutionContext& ctx) const override;

  void visit(const sub_reader& segment, PreparedStateVisitor& visitor,
             score_t boost) const override;

 private:
  States states_;
  bstring stats_;
};

}  // namespace iresearch

#endif
