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

#ifndef IRESEARCH_DISJUNCTION_H
#define IRESEARCH_DISJUNCTION_H

#include <queue>

#include "conjunction.hpp"
#include "index/iterators.hpp"
#include "utils/std.hpp"
#include "utils/type_limits.hpp"

namespace iresearch {
namespace detail {

// Need this proxy since Microsoft has heap validity check in std::pop_heap.
// Our approach is to refresh top iterator (next or seek) and then remove it
// or move to lead. So we don't need this check.
// It is quite difficult to disable check since it managed by
// _ITERATOR_DEBUG_LEVEL macros which affects ABI (it must be the same for all
// libs and objs).
template<typename Iterator, typename Pred>
FORCE_INLINE void pop_heap(Iterator first, Iterator last, Pred comp) {
  assert(first != last);  // pop requires non-empty range

#ifndef _MSC_VER
  std::pop_heap(first, last, comp);
#elif _MSC_FULL_VER < 190024000  // < MSVC2015.3
  std::_Pop_heap(std::_Unchecked(first), std::_Unchecked(last), comp);
#elif _MSC_FULL_VER < 191526726  // < MSVC2017.8
  std::_Pop_heap_unchecked(std::_Unchecked(first), std::_Unchecked(last), comp);
#else
  std::_Pop_heap_unchecked(first._Unwrapped(), last._Unwrapped(), comp);
#endif
}

template<size_t Size>
class min_match_buffer {
 public:
  explicit min_match_buffer(size_t min_match_count) noexcept
    : min_match_count_(std::max(size_t(1), min_match_count)) {}

  uint32_t match_count(size_t i) const noexcept {
    assert(i < Size);
    return match_count_[i];
  }

  bool inc(size_t i) noexcept { return ++match_count_[i] < min_match_count_; }

  void clear() noexcept { std::memset(match_count_, 0, sizeof match_count_); }

  size_t min_match_count() const noexcept { return min_match_count_; }

 private:
  const size_t min_match_count_;
  uint32_t match_count_[Size];
};

template<>
class min_match_buffer<0> {
 public:
  explicit min_match_buffer(size_t) noexcept {}
  bool inc(size_t) noexcept {
    assert(false);
    return true;
  }
  void clear() noexcept { assert(false); }
  uint32_t match_count(size_t) const noexcept {
    assert(false);
    return 1;
  }
  uint32_t min_match_count() const noexcept { return 1; }
};

class score_buffer {
 public:
  score_buffer(size_t num_buckets, size_t size)
    : bucket_size_{num_buckets * sizeof(score_t)},
      buf_size_{bucket_size_ * size},
      buf_{!buf_size_ ? nullptr : new byte_type[buf_size_]} {
    if (buf_) {
      std::memset(data(), 0, this->size());
    }
  }

  score_t* get(size_t i) noexcept {
    assert(!buf_ || bucket_size_ * i < buf_size_);
    return reinterpret_cast<score_t*>(buf_.get() + bucket_size_ * i);
  }

  score_t* data() noexcept { return reinterpret_cast<score_t*>(buf_.get()); }

  size_t size() const noexcept { return buf_size_; }

  size_t bucket_size() const noexcept { return bucket_size_; }

 private:
  size_t bucket_size_;
  size_t buf_size_;
  std::unique_ptr<byte_type[]> buf_;
};  // score_buffer

struct empty_score_buffer {
  explicit empty_score_buffer(size_t, size_t) noexcept {}

  score_t* get(size_t) noexcept {
    assert(false);
    return nullptr;
  }

  score_t* data() noexcept { return nullptr; }

  size_t size() const noexcept { return 0; }

  size_t bucket_size() const noexcept { return 0; }
};

}  // namespace detail

template<typename Adapter>
using IteratorVisitor = bool (*)(void*, Adapter&);

template<typename Adapter>
struct compound_doc_iterator : doc_iterator {
  virtual void visit(void* ctx, IteratorVisitor<Adapter>) = 0;
};

// Wrapper around regular doc_iterator to conform compound_doc_iterator API
template<typename DocIterator,
         typename Adapter = score_iterator_adapter<DocIterator>>
class unary_disjunction final : public compound_doc_iterator<Adapter> {
 public:
  using doc_iterator_t = Adapter;

  unary_disjunction(doc_iterator_t&& it) : it_(std::move(it)) {}

  virtual attribute* get_mutable(type_info::type_id type) noexcept override {
    return it_->get_mutable(type);
  }

  virtual doc_id_t value() const noexcept override { return it_.doc->value; }

  virtual bool next() override { return it_->next(); }

  virtual doc_id_t seek(doc_id_t target) override { return it_->seek(target); }

  virtual void visit(void* ctx, IteratorVisitor<Adapter> visitor) override {
    assert(ctx);
    assert(visitor);
    visitor(ctx, it_);
  }

 private:
  doc_iterator_t it_;
};  // unary_disjunction

// Disjunction optimized for two iterators.
template<typename DocIterator, typename Merger,
         typename Adapter = score_iterator_adapter<DocIterator>>
class basic_disjunction final : public compound_doc_iterator<Adapter>,
                                private Merger,
                                private score_ctx {
 public:
  using adapter = Adapter;
  using merger_type = Merger;

  basic_disjunction(adapter&& lhs, adapter&& rhs, Merger&& merger = Merger{})
    : basic_disjunction{
        std::move(lhs), std::move(rhs), std::move(merger),
        [this]() { return cost::extract(lhs_, 0) + cost::extract(rhs_, 0); },
        resolve_overload_tag{}} {}

  basic_disjunction(adapter&& lhs, adapter&& rhs, Merger&& merger,
                    cost::cost_t est)
    : basic_disjunction{std::move(lhs), std::move(rhs), std::move(merger), est,
                        resolve_overload_tag{}} {}

  virtual attribute* get_mutable(
    type_info::type_id type) noexcept override final {
    return irs::get_mutable(attrs_, type);
  }

  virtual doc_id_t value() const noexcept override {
    return std::get<document>(attrs_).value;
  }

  virtual bool next() override {
    next_iterator_impl(lhs_);
    next_iterator_impl(rhs_);

    auto& doc = std::get<document>(attrs_);
    return !doc_limits::eof(doc.value = std::min(lhs_.value(), rhs_.value()));
  }

  virtual doc_id_t seek(doc_id_t target) override {
    auto& doc = std::get<document>(attrs_);

    if (target <= doc.value) {
      return doc.value;
    }

    if (seek_iterator_impl(lhs_, target) || seek_iterator_impl(rhs_, target)) {
      return doc.value = target;
    }

    return (doc.value = std::min(lhs_.value(), rhs_.value()));
  }

  virtual void visit(void* ctx, IteratorVisitor<Adapter> visitor) override {
    assert(ctx);
    assert(visitor);

    auto& doc = std::get<document>(attrs_);
    // assume that seek or next has been called
    assert(lhs_.doc->value >= doc.value);

    if (lhs_.value() == doc.value && !visitor(ctx, lhs_)) {
      return;
    }

    seek_iterator_impl(rhs_, doc.value);
    if (rhs_.value() == doc.value) {
      visitor(ctx, rhs_);
    }
  }

 private:
  struct resolve_overload_tag {};

  template<typename Estimation>
  basic_disjunction(adapter&& lhs, adapter&& rhs, Merger&& merger,
                    Estimation&& estimation, resolve_overload_tag)
    : Merger{std::move(merger)}, lhs_(std::move(lhs)), rhs_(std::move(rhs)) {
    std::get<cost>(attrs_).reset(std::forward<Estimation>(estimation));

    if constexpr (HasScore_v<Merger>) {
      prepare_score();
    }
  }

  void prepare_score() {
    assert(Merger::size());
    assert(lhs_.score && rhs_.score);  // must be ensure by the adapter

    auto& score = std::get<irs::score>(attrs_);

    const bool lhs_score_empty = (*lhs_.score) == ScoreFunction::kDefault;
    const bool rhs_score_empty = (*rhs_.score) == ScoreFunction::kDefault;

    if (!lhs_score_empty && !rhs_score_empty) {
      // both sub-iterators have score
      score.Reset(this, [](score_ctx* ctx, score_t* res) {
        // FIXME(gnusi)
        auto& self = *static_cast<basic_disjunction*>(ctx);
        auto& merger = static_cast<Merger&>(self);
        self.score_iterator_impl(self.lhs_, res);
        self.score_iterator_impl(self.rhs_, merger.temp());
        merger(res, merger.temp());
      });
    } else if (!lhs_score_empty) {
      // only left sub-iterator has score
      score.Reset(this, [](score_ctx* ctx, score_t* res) {
        auto& self = *static_cast<basic_disjunction*>(ctx);
        return self.score_iterator_impl(self.lhs_, res);
      });
    } else if (!rhs_score_empty) {
      // only right sub-iterator has score
      score.Reset(this, [](score_ctx* ctx, score_t* res) {
        auto& self = *static_cast<basic_disjunction*>(ctx);
        return self.score_iterator_impl(self.rhs_, res);
      });
    } else {
      assert(score == ScoreFunction::kDefault);
      score = ScoreFunction::Default(Merger::size());
    }
  }

  bool seek_iterator_impl(adapter& it, doc_id_t target) {
    return it.value() < target && target == it->seek(target);
  }

  void next_iterator_impl(adapter& it) {
    auto& doc = std::get<document>(attrs_);
    const auto value = it.value();

    if (doc.value == value) {
      it->next();
    } else if (value < doc.value) {
      it->seek(doc.value + doc_id_t(!doc_limits::eof(doc.value)));
    }
  }

  void score_iterator_impl(adapter& it, score_t* res) {
    auto& doc = std::get<document>(attrs_);
    auto value = it.value();

    if (value < doc.value) {
      value = it->seek(doc.value);
    }

    if (value == doc.value) {
      (*it.score)(res);
    } else {
      std::memset(res, 0, Merger::byte_size());
    }
  }

  using attributes = std::tuple<document, score, cost>;

  mutable adapter lhs_;
  mutable adapter rhs_;
  attributes attrs_;
};

// Disjunction optimized for a small number of iterators. Implemets a linear
// search based disjunction.
// ----------------------------------------------------------------------------
//  Unscored iterators   Scored iterators
//   [0]   [1]   [2]   |   [3]    [4]     [5]
//    ^                |    ^                    ^
//    |                |    |                    |
//   begin             |   scored               end
//                     |   begin
// ----------------------------------------------------------------------------
template<typename DocIterator, typename Merger,
         typename Adapter = score_iterator_adapter<DocIterator>>
class small_disjunction final : public compound_doc_iterator<Adapter>,
                                private Merger,
                                private score_ctx {
 public:
  using adapter = Adapter;
  using doc_iterators_t = std::vector<adapter>;

  small_disjunction(doc_iterators_t&& itrs, Merger&& merger, cost::cost_t est)
    : small_disjunction{std::move(itrs), std::move(merger), est,
                        resolve_overload_tag()} {}

  explicit small_disjunction(doc_iterators_t&& itrs, Merger&& merger = Merger{})
    : small_disjunction{std::move(itrs), std::move(merger),
                        [this]() {
                          return std::accumulate(
                            begin_, end_, cost::cost_t(0),
                            [](cost::cost_t lhs, const adapter& rhs) {
                              return lhs + cost::extract(rhs, 0);
                            });
                        },
                        resolve_overload_tag()} {}

  virtual attribute* get_mutable(
    type_info::type_id type) noexcept override final {
    return irs::get_mutable(attrs_, type);
  }

  virtual doc_id_t value() const noexcept override {
    return std::get<document>(attrs_).value;
  }

  bool next_iterator_impl(adapter& it) {
    auto& doc = std::get<document>(attrs_);
    const auto value = it.value();

    if (value == doc.value) {
      return it->next();
    } else if (value < doc.value) {
      return !doc_limits::eof(it->seek(doc.value + 1));
    }

    return true;
  }

  virtual bool next() override {
    auto& doc = std::get<document>(attrs_);

    if (doc_limits::eof(doc.value)) {
      return false;
    }

    doc_id_t min = doc_limits::eof();

    for (auto begin = begin_; begin != end_;) {
      auto& it = *begin;
      if (!next_iterator_impl(it)) {
        if (!remove_iterator(begin)) {
          doc.value = doc_limits::eof();
          return false;
        }
#if defined(_MSC_VER) && defined(IRESEARCH_DEBUG)
        // workaround for Microsoft checked iterators
        begin = itrs_.begin() + std::distance(itrs_.data(), &it);
#endif
      } else {
        min = std::min(min, it.value());
        ++begin;
      }
    }

    doc.value = min;
    return true;
  }

  virtual doc_id_t seek(doc_id_t target) override {
    auto& doc = std::get<document>(attrs_);

    if (doc_limits::eof(doc.value)) {
      return doc.value;
    }

    doc_id_t min = doc_limits::eof();

    for (auto begin = begin_; begin != end_;) {
      auto& it = *begin;

      if (it.value() < target) {
        const auto value = it->seek(target);

        if (value == target) {
          return doc.value = value;
        } else if (doc_limits::eof(value)) {
          if (!remove_iterator(begin)) {
            // exhausted
            return doc.value = doc_limits::eof();
          }
#if defined(_MSC_VER) && defined(IRESEARCH_DEBUG)
          // workaround for Microsoft checked iterators
          begin = itrs_.begin() + std::distance(itrs_.data(), &it);
#endif
          continue;  // don't need to increment 'begin' here
        }
      }

      min = std::min(min, it.value());
      ++begin;
    }

    return (doc.value = min);
  }

  virtual void visit(void* ctx, IteratorVisitor<Adapter> visitor) override {
    assert(ctx);
    assert(visitor);
    auto& doc = std::get<document>(attrs_);
    hitch_all_iterators();
    for (auto begin = begin_; begin != end_; ++begin) {
      auto& it = *begin;
      if (it->value() == doc.value && !visitor(ctx, it)) {
        return;
      }
    }
  }

 private:
  struct resolve_overload_tag {};

  template<typename Estimation>
  small_disjunction(doc_iterators_t&& itrs, Merger&& merger,
                    Estimation&& estimation, resolve_overload_tag)
    : Merger{std::move(merger)},
      itrs_(itrs.size()),
      scored_begin_(itrs_.begin()),
      begin_(scored_begin_),
      end_(itrs_.end()) {
    std::get<cost>(attrs_).reset(std::forward<Estimation>(estimation));

    if (itrs_.empty()) {
      std::get<document>(attrs_).value = doc_limits::eof();
    }

    auto rbegin = itrs_.rbegin();
    for (auto& it : itrs) {
      if (*it.score == ScoreFunction::kDefault) {
        *scored_begin_ = std::move(it);
        ++scored_begin_;
      } else {
        *rbegin = std::move(it);
        ++rbegin;
      }
    }

    if constexpr (HasScore_v<Merger>) {
      prepare_score();
    }
  }

  void prepare_score() {
    assert(Merger::size());

    auto& score = std::get<irs::score>(attrs_);

    // prepare score
    if (scored_begin_ != end_) {
      score.Reset(this, [](irs::score_ctx* ctx, score_t* res) noexcept {
        auto& self = *static_cast<small_disjunction*>(ctx);
        auto& merger = static_cast<Merger&>(self);
        const auto doc = std::get<document>(self.attrs_).value;

        std::memset(res, 0, merger.byte_size());
        for (auto begin = self.scored_begin_, end = self.end_; begin != end;
             ++begin) {
          auto value = begin->value();

          if (value < doc) {
            value = (*begin)->seek(doc);
          }

          if (value == doc) {
            (*begin->score)(merger.temp());
            merger(res, merger.temp());
          }
        }
      });
    } else {
      assert(score == ScoreFunction::kDefault);
      score = ScoreFunction::Default(Merger::size());
    }
  }

  bool remove_iterator(typename doc_iterators_t::iterator it) {
    if (*it->score == ScoreFunction::kDefault) {
      std::swap(*it, *begin_);
      ++begin_;
    } else {
      std::swap(*it, *(--end_));
    }

    return begin_ != end_;
  }

  void hitch_all_iterators() {
    auto& doc = std::get<document>(attrs_);

    if (last_hitched_doc_ == doc.value) {
      return;  // nothing to do
    }
    for (auto begin = begin_; begin != end_; ++begin) {
      auto& it = *begin;
      if (it.value() < doc.value && doc_limits::eof(it->seek(doc.value))) {
#ifdef IRESEARCH_DEBUG
        assert(remove_iterator(begin));
#else
        remove_iterator(begin);
#endif
      }
    }
    last_hitched_doc_ = doc.value;
  }

  using attributes = std::tuple<document, score, cost>;
  using iterator = typename doc_iterators_t::iterator;

  doc_id_t last_hitched_doc_{doc_limits::invalid()};
  doc_iterators_t itrs_;
  iterator scored_begin_;  // beginning of scored doc iterator range
  iterator begin_;         // beginning of unscored doc iterators range
  iterator end_;           // end of scored doc iterator range
  attributes attrs_;
};

// Heapsort-based disjunction
// ----------------------------------------------------------------------------
//   [0]   <-- begin
//   [1]      |
//   [2]      | head (min doc_id heap)
//   ...      |
//   [n-1] <-- end
//   [n]   <-- lead (accepted iterator)
// ----------------------------------------------------------------------------
template<typename DocIterator, typename Merger,
         typename Adapter = score_iterator_adapter<DocIterator>,
         bool EnableUnary = false>
class disjunction final : public compound_doc_iterator<Adapter>,
                          private Merger,
                          private score_ctx {
 public:
  using adapter = Adapter;
  using merger_type = Merger;
  using doc_iterators_t = std::vector<adapter>;
  using heap_container = std::vector<size_t>;
  using heap_iterator = heap_container::iterator;

  static constexpr bool kEnableUnary = EnableUnary;
  static constexpr size_t kSmallDisjunctionUpperBound = 5;

  disjunction(doc_iterators_t&& itrs, Merger&& merger, cost::cost_t est)
    : disjunction{std::move(itrs), std::move(merger), est,
                  resolve_overload_tag()} {}

  explicit disjunction(doc_iterators_t&& itrs, Merger&& merger = Merger{})
    : disjunction{std::move(itrs), std::move(merger),
                  [this]() {
                    return std::accumulate(
                      itrs_.begin(), itrs_.end(), cost::cost_t(0),
                      [](cost::cost_t lhs, const adapter& rhs) {
                        return lhs + cost::extract(rhs, 0);
                      });
                  },
                  resolve_overload_tag{}} {}

  virtual attribute* get_mutable(
    type_info::type_id type) noexcept override final {
    return irs::get_mutable(attrs_, type);
  }

  virtual doc_id_t value() const noexcept override {
    return std::get<document>(attrs_).value;
  }

  virtual bool next() override {
    auto& doc = std::get<document>(attrs_);

    if (doc_limits::eof(doc.value)) {
      return false;
    }

    while (lead().value() <= doc.value) {
      bool const exhausted = lead().value() == doc.value
                               ? !lead()->next()
                               : doc_limits::eof(lead()->seek(doc.value + 1));

      if (exhausted && !remove_lead()) {
        doc.value = doc_limits::eof();
        return false;
      } else {
        refresh_lead();
      }
    }

    doc.value = lead().value();

    return true;
  }

  virtual doc_id_t seek(doc_id_t target) override {
    auto& doc = std::get<document>(attrs_);

    if (doc_limits::eof(doc.value)) {
      return doc.value;
    }

    while (lead().value() < target) {
      const auto value = lead()->seek(target);

      if (doc_limits::eof(value) && !remove_lead()) {
        return doc.value = doc_limits::eof();
      } else if (value != target) {
        refresh_lead();
      }
    }

    return doc.value = lead().value();
  }

  virtual void visit(void* ctx, IteratorVisitor<Adapter> visitor) override {
    assert(ctx);
    assert(visitor);
    if (heap_.empty()) {
      return;
    }
    hitch_all_iterators();
    auto& lead = itrs_[heap_.back()];
    auto cont = visitor(ctx, lead);
    if (cont && heap_.size() > 1) {
      auto value = lead.value();
      irstd::heap::for_each_if(
        heap_.cbegin(), heap_.cend() - 1,
        [this, value, &cont](const size_t it) {
          assert(it < itrs_.size());
          return cont && itrs_[it].value() == value;
        },
        [this, ctx, visitor, &cont](const size_t it) {
          assert(it < itrs_.size());
          cont = visitor(ctx, itrs_[it]);
        });
    }
  }

 private:
  struct resolve_overload_tag {};

  using attributes = std::tuple<document, score, cost>;

  template<typename Estimation>
  disjunction(doc_iterators_t&& itrs, Merger&& merger, Estimation&& estimation,
              resolve_overload_tag)
    : Merger{std::move(merger)}, itrs_{std::move(itrs)} {
    // since we are using heap in order to determine next document,
    // in order to avoid useless make_heap call we expect that all
    // iterators are equal here */
    // assert(irstd::all_equal(itrs_.begin(), itrs_.end()));
    std::get<cost>(attrs_).reset(std::forward<Estimation>(estimation));

    if (itrs_.empty()) {
      std::get<document>(attrs_).value = doc_limits::eof();
    }

    // prepare external heap
    heap_.resize(itrs_.size());
    std::iota(heap_.begin(), heap_.end(), size_t(0));

    if constexpr (HasScore_v<Merger>) {
      prepare_score();
    }
  }

  void prepare_score() {
    assert(Merger::size());

    auto& score = std::get<irs::score>(attrs_);

    score.Reset(this, [](score_ctx* ctx, score_t* res) {
      auto& self = *static_cast<disjunction*>(ctx);
      assert(!self.heap_.empty());

      const auto its = self.hitch_all_iterators();

      if (auto& score = *self.lead().score; !score.IsNoop()) {
        score(res);
      } else {
        std::memset(res, 0, self.byte_size());
      }
      if (const auto doc = std::get<document>(self.attrs_).value;
          self.top().value() == doc) {
        irstd::heap::for_each_if(
          its.first, its.second,
          [&self, doc](const size_t it) noexcept {
            assert(it < self.itrs_.size());
            return self.itrs_[it].value() == doc;
          },
          [&self, res](size_t it) {
            assert(it < self.itrs_.size());
            if (auto& score = *self.itrs_[it].score; !score.IsNoop()) {
              auto& merger = static_cast<Merger&>(self);
              score(merger.temp());
              merger(res, merger.temp());
            }
          });
      }
    });
  }

  template<typename Iterator>
  inline void push(Iterator begin, Iterator end) noexcept {
    // lambda here gives ~20% speedup on GCC
    std::push_heap(begin, end,
                   [this](const size_t lhs, const size_t rhs) noexcept {
                     assert(lhs < itrs_.size());
                     assert(rhs < itrs_.size());
                     return itrs_[lhs].value() > itrs_[rhs].value();
                   });
  }

  template<typename Iterator>
  inline void pop(Iterator begin, Iterator end) noexcept {
    // lambda here gives ~20% speedup on GCC
    detail::pop_heap(begin, end,
                     [this](const size_t lhs, const size_t rhs) noexcept {
                       assert(lhs < itrs_.size());
                       assert(rhs < itrs_.size());
                       return itrs_[lhs].value() > itrs_[rhs].value();
                     });
  }

  // Removes lead iterator.
  // Returns true - if the disjunction condition still can be satisfied,
  // false - otherwise.
  inline bool remove_lead() noexcept {
    heap_.pop_back();

    if (!heap_.empty()) {
      pop(heap_.begin(), heap_.end());
      return true;
    }

    return false;
  }

  inline void refresh_lead() noexcept {
    auto begin = heap_.begin(), end = heap_.end();
    push(begin, end);
    pop(begin, end);
  }

  inline adapter& lead() noexcept {
    assert(!heap_.empty());
    assert(heap_.back() < itrs_.size());
    return itrs_[heap_.back()];
  }

  inline adapter& top() noexcept {
    assert(!heap_.empty());
    assert(heap_.front() < itrs_.size());
    return itrs_[heap_.front()];
  }

  std::pair<heap_iterator, heap_iterator> hitch_all_iterators() {
    // hitch all iterators in head to the lead (current doc_)
    assert(!heap_.empty());
    auto begin = heap_.begin(), end = heap_.end() - 1;

    auto& doc = std::get<document>(attrs_);
    while (begin != end && top().value() < doc.value) {
      const auto value = top()->seek(doc.value);

      if (doc_limits::eof(value)) {
        // remove top
        pop(begin, end);
        std::swap(*--end, heap_.back());
        heap_.pop_back();
      } else {
        // refresh top
        pop(begin, end);
        push(begin, end);
      }
    }
    return {begin, end};
  }

  doc_iterators_t itrs_;
  heap_container heap_;
  attributes attrs_;
};  // disjunction

enum class MatchType { kMatch, kMinMatchFast, kMinMatch };

template<MatchType MinMatch, bool SeekReadahead, size_t NumBlocks = 8>
struct block_disjunction_traits {
  // "false" - iterator is used for min match filtering,
  // "true" - otherwise
  static constexpr bool kMinMatch = MatchType::kMatch != MinMatch;

  // "false" - iterator is used for min match filtering,
  // "true" - otherwise
  static constexpr bool kMinMatchEarlyPruning =
    MatchType::kMinMatchFast == MinMatch;

  // Use readahead buffer for random access
  static constexpr bool kSeekReadahead = SeekReadahead;

  // Size of the readhead buffer in blocks
  static constexpr size_t kNumBlocks = NumBlocks;
};

// The implementation reads ahead 64*NumBlocks documents.
// It isn't optimized for conjunction case when the requected min match
// count equals to a number of input iterators.
// It's better to to use a dedicated "conjunction" iterator.
template<typename DocIterator, typename Merger, typename Traits,
         typename Adapter = score_iterator_adapter<DocIterator>>
class block_disjunction final : public doc_iterator,
                                private Merger,
                                private score_ctx {
 public:
  using traits_type = Traits;
  using adapter = Adapter;
  using merger_type = Merger;
  using doc_iterators_t = std::vector<adapter>;

  // FIXME(gnusi): do we need this?
  static constexpr bool kEnableUnary = false;

  // Block disjunction is faster than small_disjunction
  static constexpr size_t kSmallDisjunctionUpperBound = 0;

  block_disjunction(doc_iterators_t&& itrs, Merger&& merger, cost::cost_t est)
    : block_disjunction{std::move(itrs), 1, std::move(merger), est} {}

  block_disjunction(doc_iterators_t&& itrs, size_t min_match_count,
                    Merger&& merger, cost::cost_t est)
    : block_disjunction{std::move(itrs), min_match_count, std::move(merger),
                        est, resolve_overload_tag()} {}

  explicit block_disjunction(doc_iterators_t&& itrs, Merger&& merger = Merger{})
    : block_disjunction{std::move(itrs), 1, std::move(merger)} {}

  block_disjunction(doc_iterators_t&& itrs, size_t min_match_count,
                    Merger&& merger = Merger{})
    : block_disjunction{std::move(itrs), min_match_count, std::move(merger),
                        [this]() {
                          return std::accumulate(
                            itrs_.begin(), itrs_.end(), cost::cost_t(0),
                            [](cost::cost_t lhs, const adapter& rhs) {
                              return lhs + cost::extract(rhs, 0);
                            });
                        },
                        resolve_overload_tag()} {}

  size_t match_count() const noexcept { return match_count_; }

  virtual attribute* get_mutable(
    type_info::type_id type) noexcept override final {
    return irs::get_mutable(attrs_, type);
  }

  virtual doc_id_t value() const noexcept override {
    return std::get<document>(attrs_).value;
  }

  virtual bool next() override {
    auto& doc = std::get<document>(attrs_);

    do {
      while (!cur_) {
        if (begin_ >= std::end(mask_)) {
          if (refill()) {
            assert(cur_);
            break;
          }

          doc.value = doc_limits::eof();
          match_count_ = 0;

          return false;
        }

        cur_ = *begin_++;
        doc_base_ += bits_required<uint64_t>();
        if constexpr (traits_type::kMinMatch || HasScore_v<Merger>) {
          buf_offset_ += bits_required<uint64_t>();
        }
      }

      const size_t offset = std::countr_zero(cur_);
      irs::unset_bit(cur_, offset);

      [[maybe_unused]] const size_t buf_offset = buf_offset_ + offset;

      if constexpr (traits_type::kMinMatch) {
        match_count_ = match_buf_.match_count(buf_offset);

        if (match_count_ < match_buf_.min_match_count()) {
          continue;
        }
      }

      doc.value = doc_base_ + doc_id_t(offset);
      if constexpr (HasScore_v<Merger>) {
        score_value_ = score_buf_.get(buf_offset);
      }

      return true;
    } while (traits_type::kMinMatch);

    assert(false);
    return true;
  }

  virtual doc_id_t seek(doc_id_t target) override {
    auto& doc = std::get<document>(attrs_);

    if (target <= doc.value) {
      return doc.value;
    } else if (target < max_) {
      const doc_id_t block_base = (max_ - kWindow);

      target -= block_base;
      const doc_id_t block_offset = target / kBlockSize;

      doc_base_ = block_base + block_offset * kBlockSize;
      begin_ = mask_ + block_offset + 1;

      assert(begin_ > std::begin(mask_) && begin_ <= std::end(mask_));
      cur_ = begin_[-1] & ((~UINT64_C(0)) << target % kBlockSize);

      next();
    } else {
      doc.value = doc_limits::eof();

      if constexpr (traits_type::kMinMatch) {
        match_count_ = 0;
      }

      visit_and_purge([this, target, &doc](auto& it) mutable {
        UNUSED(this);
        const auto value = it->seek(target);

        if (doc_limits::eof(value)) {
          // exhausted
          return false;
        }

        // this is to circumvent bug in GCC 10.1 on ARM64
        constexpr bool is_min_match = traits_type::kMinMatch;
        if (value < doc.value) {
          doc.value = value;
          if constexpr (is_min_match) {
            match_count_ = 1;
          }
        } else {
          if constexpr (is_min_match) {
            if (target == value) {
              ++match_count_;
            }
          }
        }

        return true;
      });

      if (itrs_.empty()) {
        doc.value = doc_limits::eof();
        match_count_ = 0;

        return doc_limits::eof();
      }

      assert(!doc_limits::eof(doc.value));
      cur_ = 0;
      begin_ = std::end(mask_);  // enforce "refill()" for upcoming "next()"
      max_ = doc.value;

      if constexpr (traits_type::kSeekReadahead) {
        min_ = doc.value;
        next();
      } else {
        min_ = doc.value + 1;
        buf_offset_ = 0;

        if constexpr (traits_type::kMinMatch) {
          if (match_count_ < match_buf_.min_match_count()) {
            next();
            return doc.value;
          }
        }

        if constexpr (HasScore_v<Merger>) {
          std::memset(score_buf_.data(), 0, score_buf_.bucket_size());
          for (auto& it : itrs_) {
            assert(it.score);
            if (*it.score != ScoreFunction::kDefault &&
                doc.value == it->value()) {
              auto& merger = static_cast<Merger&>(*this);
              (*it.score)(merger.temp());
              merger(score_buf_.data(), merger.temp());
            }
          }

          score_value_ = score_buf_.data();
        }
      }
    }

    return doc.value;
  }

 private:
  static constexpr doc_id_t kBlockSize = bits_required<uint64_t>();

  static constexpr doc_id_t kNumBlocks =
    static_cast<doc_id_t>(std::max(size_t(1), traits_type::kNumBlocks));

  static constexpr doc_id_t kWindow = kBlockSize * kNumBlocks;

  static_assert(kBlockSize * size_t(kNumBlocks) <
                std::numeric_limits<doc_id_t>::max());

  // FIXME(gnusi): stack based score_buffer for constant cases
  using score_buffer_type =
    std::conditional_t<HasScore_v<Merger>, detail::score_buffer,
                       detail::empty_score_buffer>;

  using min_match_buffer_type =
    detail::min_match_buffer<traits_type::kMinMatch ? kWindow : 0>;

  using attributes = std::tuple<document, score, cost>;

  struct resolve_overload_tag {};

  template<typename Estimation>
  block_disjunction(doc_iterators_t&& itrs, size_t min_match_count,
                    Merger&& merger, Estimation&& estimation,
                    resolve_overload_tag)
    : Merger{std::move(merger)},
      itrs_(std::move(itrs)),
      match_count_(itrs_.empty()
                     ? size_t(0)
                     : static_cast<size_t>(!traits_type::kMinMatch)),
      score_buf_(Merger::size(), kWindow),
      match_buf_(min_match_count) {
    std::get<cost>(attrs_).reset(std::forward<Estimation>(estimation));

    if (itrs_.empty()) {
      std::get<document>(attrs_).value = doc_limits::eof();
    }

    if constexpr (HasScore_v<Merger>) {
      assert(Merger::size());
      auto& score = std::get<irs::score>(attrs_);

      score.Reset(this, [](score_ctx* ctx, score_t* res) noexcept {
        auto& self = static_cast<block_disjunction&>(*ctx);
        // FIXME(gnusi)
        std::memcpy(res, self.score_value_,
                    static_cast<Merger&>(self).byte_size());
      });
    }

    if (traits_type::kMinMatch && min_match_count > 1) {
      // sort subnodes in ascending order by their cost
      // FIXME(gnusi) don't use extract
      std::sort(std::begin(itrs_), std::end(itrs_),
                [](const adapter& lhs, const adapter& rhs) {
                  return cost::extract(lhs, 0) < cost::extract(rhs, 0);
                });

      // FIXME(gnusi): fix estimation, we have to estimate only min_match
      // iterators
    }
  }

  template<typename Visitor>
  void visit_and_purge(Visitor visitor) {
    auto* begin = itrs_.data();
    auto* end = itrs_.data() + itrs_.size();

    while (begin != end) {
      if (!visitor(*begin)) {
        irstd::swap_remove(itrs_, begin);
        --end;

        if constexpr (traits_type::kMinMatchEarlyPruning) {
          // we don't need precise match count
          if (itrs_.size() < match_buf_.min_match_count()) {
            // can't fulfill min match requirement anymore
            itrs_.clear();
            return;
          }
        }
      } else {
        ++begin;
      }
    }

    if constexpr (traits_type::kMinMatch &&
                  !traits_type::kMinMatchEarlyPruning) {
      // we need precise match count, so can't break earlier
      if (itrs_.size() < match_buf_.min_match_count()) {
        // can't fulfill min match requirement anymore
        itrs_.clear();
        return;
      }
    }
  }

  void reset() noexcept {
    std::memset(mask_, 0, sizeof mask_);
    if constexpr (HasScore_v<Merger>) {
      score_value_ = score_buf_.data();
      std::memset(score_buf_.data(), 0, score_buf_.size());
    }
    if constexpr (traits_type::kMinMatch) {
      match_buf_.clear();
    }
  }

  bool refill() {
    if (itrs_.empty()) {
      return false;
    }

    if constexpr (!traits_type::kMinMatch) {
      reset();
    }

    bool empty = true;

    do {
      if constexpr (traits_type::kMinMatch) {
        // in min match case we need to clear
        // internal buffers on every iteration
        reset();
      }

      doc_base_ = min_;
      max_ = min_ + kWindow;
      min_ = doc_limits::eof();

      visit_and_purge([this, &empty](auto& it) mutable {
        // FIXME
        // for min match case we can skip the whole block if
        // we can't satisfy match_buf_.min_match_count() conditions, namely
        // if constexpr (traits_type::kMinMatch) {
        //  if (empty && (&it + (match_buf_.min_match_count() -
        //  match_buf_.max_match_count()) < (itrs_.data() + itrs_.size()))) {
        //    // skip current block
        //    return true;
        //  }
        //}

        if constexpr (HasScore_v<Merger>) {
          assert(Merger::size());
          if (it.score->Func() != irs::ScoreFunction::kDefault) {
            return this->refill<true>(it, empty);
          }
        }

        return this->refill<false>(it, empty);
      });
    } while (empty && !itrs_.empty());

    if (empty) {
      // exhausted
      assert(itrs_.empty());
      return false;
    }

    cur_ = *mask_;
    begin_ = mask_ + 1;
    if constexpr (traits_type::kMinMatch || HasScore_v<Merger>) {
      buf_offset_ = 0;
    }
    while (!cur_) {
      cur_ = *begin_++;
      doc_base_ += bits_required<uint64_t>();
      if constexpr (traits_type::kMinMatch || HasScore_v<Merger>) {
        buf_offset_ += bits_required<uint64_t>();
      }
    }
    assert(cur_);

    return true;
  }

  template<bool Score>
  bool refill(adapter& it, bool& empty) {
    assert(it.doc);
    const auto* doc = &it.doc->value;
    assert(!doc_limits::eof(*doc));

    // disjunction is 1 step next behind, that may happen:
    // - before the very first next()
    // - after seek() in case of 'kSeekReadahead == false'
    if (*doc < doc_base_ && !it->next()) {
      // exhausted
      return false;
    }

    for (;;) {
      if (*doc >= max_) {
        min_ = std::min(*doc, min_);
        return true;
      }

      const size_t offset = *doc - doc_base_;

      irs::set_bit(mask_[offset / kBlockSize], offset % kBlockSize);

      if constexpr (Score) {
        assert(it.score);
        auto& merger = static_cast<Merger&>(*this);
        (*it.score)(merger.temp());
        merger(score_buf_.get(offset), merger.temp());
      }

      if constexpr (traits_type::kMinMatch) {
        empty &= match_buf_.inc(offset);
      } else {
        empty = false;
      }

      if (!it->next()) {
        // exhausted
        return false;
      }
    }
  }

  uint64_t mask_[kNumBlocks]{};
  doc_iterators_t itrs_;
  uint64_t* begin_{std::end(mask_)};
  uint64_t cur_{};
  doc_id_t doc_base_{doc_limits::invalid()};
  doc_id_t min_{doc_limits::min()};      // base doc id for the next mask
  doc_id_t max_{doc_limits::invalid()};  // max doc id in the current mask
  attributes attrs_;
  size_t match_count_;
  size_t buf_offset_{};  // offset within a buffer
  IRS_NO_UNIQUE_ADDRESS score_buffer_type score_buf_;
  IRS_NO_UNIQUE_ADDRESS min_match_buffer_type match_buf_;
  const score_t* score_value_{score_buf_.data()};
};  // block_disjunction

template<typename DocIterator, typename Merger,
         typename Adapter = score_iterator_adapter<DocIterator>>
using disjunction_iterator =
  block_disjunction<DocIterator, Merger,
                    block_disjunction_traits<MatchType::kMatch, false>,
                    Adapter>;

template<typename DocIterator, typename Merger,
         typename Adapter = score_iterator_adapter<DocIterator>>
using min_match_iterator =
  block_disjunction<DocIterator, Merger,
                    block_disjunction_traits<MatchType::kMinMatch, false>,
                    Adapter>;

template<typename T>
struct RebindIterator;

template<typename DocIterator, typename Merger, typename Adapter,
         bool EnableUnary>
struct RebindIterator<disjunction<DocIterator, Merger, Adapter, EnableUnary>> {
  using Unary = unary_disjunction<DocIterator, Adapter>;
  using Basic = basic_disjunction<DocIterator, Merger, Adapter>;
  using Small = small_disjunction<DocIterator, Merger, Adapter>;
};

template<typename DocIterator, typename Merger, typename Adapter>
struct RebindIterator<disjunction_iterator<DocIterator, Merger, Adapter>> {
  using Unary = unary_disjunction<DocIterator, Adapter>;
  using Basic = basic_disjunction<DocIterator, Merger, Adapter>;
  using Small = disjunction_iterator<DocIterator, Merger, Adapter>;
};

template<typename DocIterator, typename Merger, typename Adapter>
struct RebindIterator<min_match_iterator<DocIterator, Merger, Adapter>> {
  using Disjunction = disjunction_iterator<DocIterator, Merger, Adapter>;
  using Conjunction = conjunction<DocIterator, Merger>;
};

// Returns disjunction iterator created from the specified sub iterators
template<typename Disjunction, typename Merger, typename... Args>
doc_iterator::ptr MakeDisjunction(typename Disjunction::doc_iterators_t&& itrs,
                                  Merger&& merger, Args&&... args) {
  const auto size = itrs.size();

  if (0 == size) {
    // Empty or unreachable search criteria
    return doc_iterator::empty();
  }

  if (1 == size) {
    // Single sub-query
    if constexpr (Disjunction::kEnableUnary) {
      using UnaryDisjunction = typename RebindIterator<Disjunction>::Unary;

      return memory::make_managed<UnaryDisjunction>(std::move(itrs.front()));
    }

    return std::move(itrs.front());
  }

  if (2 == size) {
    // 2-way disjunction
    using BasicDisjunction = typename RebindIterator<Disjunction>::Basic;

    return memory::make_managed<BasicDisjunction>(
      std::move(itrs.front()), std::move(itrs.back()),
      std::forward<Merger>(merger), std::forward<Args>(args)...);
  }

  if constexpr (Disjunction::kSmallDisjunctionUpperBound > 0) {
    if (size <= Disjunction::kSmallDisjunctionUpperBound) {
      using SmallDisjunction = typename RebindIterator<Disjunction>::Small;

      return memory::make_managed<SmallDisjunction>(
        std::move(itrs), std::forward<Merger>(merger),
        std::forward<Args>(args)...);
    }
  }

  return memory::make_managed<Disjunction>(
    std::move(itrs), std::forward<Merger>(merger), std::forward<Args>(args)...);
}

// Returns weak conjunction iterator created from the specified sub iterators
template<typename WeakConjunction, typename Merger, typename... Args>
doc_iterator::ptr MakeWeakDisjunction(
  typename WeakConjunction::doc_iterators_t&& itrs, size_t min_match,
  Merger&& merger, Args&&... args) {
  // This case must be handled by a caller, we're unable to process it here
  assert(min_match > 0);

  const auto size = itrs.size();

  if (0 == size || min_match > size) {
    // Empty or unreachable search criteria
    return doc_iterator::empty();
  }

  if (1 == min_match) {
    // Pure disjunction
    assert(size >= min_match);
    using Disjunction = typename RebindIterator<WeakConjunction>::Disjunction;

    return MakeDisjunction<Disjunction>(std::move(itrs),
                                        std::forward<Merger>(merger),
                                        std::forward<Args>(args)...);
  }

  if (min_match == size) {
    // Pure conjunction
    assert(min_match == size);
    using Conjunction = typename RebindIterator<WeakConjunction>::Conjunction;

    return memory::make_managed<Conjunction>(
      typename Conjunction::doc_iterators_t{
        std::make_move_iterator(std::begin(itrs)),
        std::make_move_iterator(std::end(itrs))},
      std::forward<Merger>(merger));
  }

  return memory::make_managed<WeakConjunction>(std::move(itrs), min_match,
                                               std::forward<Merger>(merger),
                                               std::forward<Args>(args)...);
}

}  // namespace iresearch

#endif  // IRESEARCH_DISJUNCTION_H
