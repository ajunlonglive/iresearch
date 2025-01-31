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

#include "assert_format.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <unordered_set>

#include "analysis/token_attributes.hpp"
#include "analysis/token_stream.hpp"
#include "index/comparer.hpp"
#include "index/directory_reader.hpp"
#include "index/field_meta.hpp"
#include "search/boolean_filter.hpp"
#include "search/cost.hpp"
#include "search/score.hpp"
#include "search/term_filter.hpp"
#include "search/tfidf.hpp"
#include "store/data_output.hpp"
#include "tests_shared.hpp"
#include "utils/automaton_utils.hpp"
#include "utils/bit_utils.hpp"
#include "utils/fstext/fst_table_matcher.hpp"

namespace {

bool visit(const irs::column_reader& reader,
           const std::function<bool(irs::doc_id_t, irs::bytes_view)>& visitor) {
  auto it = reader.iterator(irs::ColumnHint::kConsolidation);

  irs::payload dummy;
  auto* doc = irs::get<irs::document>(*it);
  if (!doc) {
    return false;
  }
  auto* payload = irs::get<irs::payload>(*it);
  if (!payload) {
    payload = &dummy;
  }

  while (it->next()) {
    if (!visitor(doc->value, payload->value)) {
      return false;
    }
  }

  return true;
}

}  // namespace

namespace tests {

void assert_term(const irs::term_iterator& expected_term,
                 const irs::term_iterator& actual_term,
                 irs::IndexFeatures requested_features);

void posting::insert(uint32_t pos, uint32_t offs_start,
                     const irs::attribute_provider& attrs) {
  auto* offs = irs::get<irs::offset>(attrs);
  auto* pay = irs::get<irs::payload>(attrs);

  uint32_t start = std::numeric_limits<uint32_t>::max();
  uint32_t end = std::numeric_limits<uint32_t>::max();
  if (offs) {
    start = offs_start + offs->start;
    end = offs_start + offs->end;
  }

  positions_.emplace(pos, start, end, pay ? pay->value : irs::bytes_view{});
}

posting& term::insert(irs::doc_id_t id) {
  return const_cast<posting&>(*postings.emplace(id).first);
}

term::term(irs::bytes_view data) : value(data) {}

bool term::operator<(const term& rhs) const {
  return irs::memcmp_less(value.c_str(), value.size(), rhs.value.c_str(),
                          rhs.value.size());
}

void term::sort(const std::map<irs::doc_id_t, irs::doc_id_t>& docs) {
  std::set<posting> resorted_postings;

  for (auto& posting : postings) {
    resorted_postings.emplace(
      docs.at(posting.id_),
      std::move(const_cast<tests::posting&>(posting).positions_));
  }

  postings = std::move(resorted_postings);
}

field::field(const std::string_view& name, irs::IndexFeatures index_features,
             const irs::features_t& features)
  : field_meta(name, index_features), stats{} {
  for (const auto feature : features) {
    this->features[feature] = irs::field_limits::invalid();
  }
}

term& field::insert(irs::bytes_view t) {
  auto res = terms.emplace(t);
  return const_cast<term&>(*res.first);
}

term* field::find(irs::bytes_view t) {
  auto it = terms.find(term(t));
  return terms.end() == it ? nullptr : const_cast<term*>(&*it);
}

size_t field::remove(irs::bytes_view t) { return terms.erase(term(t)); }

irs::bytes_view field::min() const {
  EXPECT_FALSE(terms.empty());
  return std::begin(terms)->value;
}

irs::bytes_view field::max() const {
  EXPECT_FALSE(terms.empty());
  return std::rbegin(terms)->value;
}

uint64_t field::total_freq() const {
  using freq_t = decltype(irs::frequency{}.value);
  static_assert(std::is_unsigned_v<freq_t>);

  freq_t value{0};
  for (auto& term : terms) {
    for (auto& post : term.postings) {
      const auto sum = value + post.positions().size();
      EXPECT_GE(sum, value);
      EXPECT_GE(sum, post.positions().size());
      value += static_cast<freq_t>(post.positions().size());
    }
  }

  return value;
}

void field::sort(const std::map<irs::doc_id_t, irs::doc_id_t>& docs) {
  for (auto& term : terms) {
    const_cast<tests::term&>(term).sort(docs);
  }
}

void column_values::insert(irs::doc_id_t key, irs::bytes_view value) {
  ASSERT_TRUE(irs::doc_limits::valid(key));
  ASSERT_TRUE(!irs::doc_limits::eof(key));

  const auto res = values_.emplace(key, value);

  if (!res.second) {
    res.first->second.append(value.data(), value.size());
  }
}

irs::bstring column_values::payload() const {
  if (!payload_.has_value() && writer_) {
    payload_.emplace();
    writer_->finish(payload_.value());
  }

  return payload_.value_or(irs::bstring{});
}

void column_values::sort(const std::map<irs::doc_id_t, irs::doc_id_t>& docs) {
  std::map<irs::doc_id_t, irs::bstring> resorted_values;

  for (auto& value : values_) {
    resorted_values.emplace(docs.at(value.first), std::move(value.second));
  }

  values_ = std::move(resorted_values);
}

void column_values::rewrite() {
  if (writer_ && factory_) {
    irs::bstring hdr = payload();
    const irs::bytes_view ref = hdr;
    auto writer = factory_({&ref, 1});
    ASSERT_NE(nullptr, writer);

    std::map<irs::doc_id_t, irs::bstring> values;
    for (auto& value : values_) {
      irs::bytes_output out{values[value.first]};
      writer->write(out, value.second);
    }

    ASSERT_TRUE(payload_.has_value());
    payload_.value().clear();
    writer->finish(payload_.value());
    values_ = std::move(values);
  }
}

void index_segment::compute_features() {
  struct column_output final : public irs::column_output {
   public:
    explicit column_output(irs::bstring& buf) noexcept : buf_{&buf} {}

    column_output(column_output&&) = default;
    column_output& operator=(column_output&&) = default;

    virtual void write_byte(irs::byte_type b) override { (*buf_) += b; }

    virtual void write_bytes(const irs::byte_type* b, size_t size) override {
      buf_->append(b, size);
    }

    virtual void reset() override { buf_->clear(); }

    irs::bstring* buf_;
  } out{buf_};

  bool written{};
  irs::columnstore_writer::values_writer_f writer =
    [&out, &written](irs::doc_id_t) -> column_output& {
    written = true;
    return out;
  };

  for (auto* field : doc_fields_) {
    for (auto& entry : field->feature_infos) {
      if (entry.writer) {
        buf_.clear();

        const auto doc_id = doc();
        written = false;
        entry.writer->write(field->stats, doc_id, writer);

        if (written) {
          columns_[entry.id].insert(doc_id, buf_);
        }
      }
    }
  }
}

void index_segment::insert_sorted(const ifield& f) {
  buf_.clear();
  irs::bytes_output out{buf_};
  if (f.write(out)) {
    sort_.emplace_back(std::move(buf_), doc());
  }
}

void index_segment::insert_stored(const ifield& f) {
  buf_.clear();
  irs::bytes_output out{buf_};
  if (!f.write(out)) {
    return;
  }

  const size_t id = columns_.size();
  EXPECT_LE(id, std::numeric_limits<irs::field_id>::max());

  auto res =
    named_columns_.emplace(static_cast<std::string>(f.name()), nullptr);

  if (res.second) {
    res.first->second =
      &columns_.emplace_back(static_cast<std::string>(f.name()), id);
  }

  auto* column = res.first->second;
  ASSERT_NE(nullptr, column);
  EXPECT_LT(column->id(), columns_.size());
  column->insert(doc(), buf_);
}

void index_segment::insert_indexed(const ifield& f) {
  const std::string_view field_name = f.name();

  const auto requested_features = f.index_features();
  const auto features = requested_features &
                        (~(irs::IndexFeatures::OFFS | irs::IndexFeatures::PAY));

  const auto res =
    fields_.emplace(field_name, field{field_name, features, f.features()});

  field& field = res.first->second;

  if (res.second) {
    auto& new_field = res.first->second;
    id_to_field_.emplace_back(&new_field);
    for (auto& feature : new_field.features) {
      auto handler = field_features_(feature.first);

      auto feature_writer = handler.second ? (*handler.second)({}) : nullptr;

      if (feature_writer) {
        const size_t id = columns_.size();
        ASSERT_LE(id, std::numeric_limits<irs::field_id>::max());
        columns_.emplace_back(id, handler.second, feature_writer.get());

        feature.second = irs::field_id{id};

        new_field.feature_infos.emplace_back(field::feature_info{
          irs::field_id{id}, handler.second, std::move(feature_writer)});
      }
    }

    const_cast<std::string_view&>(res.first->first) = field.name;
  }

  doc_fields_.insert(&field);

  auto& stream = f.get_tokens();

  auto* term = irs::get<irs::term_attribute>(stream);
  assert(term);
  auto* inc = irs::get<irs::increment>(stream);
  assert(inc);
  auto* offs = irs::get<irs::offset>(stream);
  if (irs::IndexFeatures::OFFS ==
        (requested_features & irs::IndexFeatures::OFFS) &&
      offs) {
    field.index_features |= irs::IndexFeatures::OFFS;
  }
  auto* pay = irs::get<irs::payload>(stream);
  if (irs::IndexFeatures::PAY ==
        (requested_features & irs::IndexFeatures::PAY) &&
      pay) {
    field.index_features |= irs::IndexFeatures::PAY;
  }

  bool empty = true;
  const auto doc_id = doc();

  while (stream.next()) {
    tests::term& trm = field.insert(term->value);

    if (trm.postings.empty() ||
        std::prev(std::end(trm.postings))->id() != doc_id) {
      ++field.stats.num_unique;
    }

    tests::posting& pst = trm.insert(doc_id);
    field.stats.pos += inc->value;
    field.stats.num_overlap += static_cast<uint32_t>(0 == inc->value);
    ++field.stats.len;
    pst.insert(field.stats.pos, field.stats.offs, stream);
    field.stats.max_term_freq = std::max(
      field.stats.max_term_freq,
      static_cast<decltype(field.stats.max_term_freq)>(pst.positions().size()));

    empty = false;
  }

  if (!empty) {
    field.docs.emplace(doc_id);
  }

  if (offs) {
    field.stats.offs += offs->end;
  }
}

void index_segment::sort(const irs::comparer& comparator) {
  if (sort_.empty()) {
    return;
  }

  std::sort(sort_.begin(), sort_.end(),
            [&comparator](const std::pair<irs::bstring, irs::doc_id_t>& lhs,
                          const std::pair<irs::bstring, irs::doc_id_t>& rhs) {
              return comparator(lhs.first, rhs.first);
            });

  irs::doc_id_t new_doc_id = irs::doc_limits::min();
  std::map<irs::doc_id_t, irs::doc_id_t> order;
  for (auto& entry : sort_) {
    order[entry.second] = new_doc_id++;
  }

  for (auto& field : fields_) {
    field.second.sort(order);
  }

  for (auto& column : columns_) {
    column.sort(order);
  }
}

class doc_iterator : public irs::doc_iterator {
 public:
  doc_iterator(irs::IndexFeatures features, const tests::term& data);

  irs::doc_id_t value() const override { return doc_.value; }

  irs::attribute* get_mutable(irs::type_info::type_id type) noexcept override {
    const auto it = attrs_.find(type);
    return it == attrs_.end() ? nullptr : it->second;
  }

  virtual bool next() override {
    if (next_ == data_.postings.end()) {
      doc_.value = irs::doc_limits::eof();
      return false;
    }

    prev_ = next_, ++next_;
    doc_.value = prev_->id();
    freq_.value = static_cast<uint32_t>(prev_->positions().size());
    pos_.clear();

    return true;
  }

  virtual irs::doc_id_t seek(irs::doc_id_t id) override {
    auto it = data_.postings.find(posting{id});

    if (it == data_.postings.end()) {
      prev_ = next_ = it;
      return irs::doc_limits::eof();
    }

    prev_ = it;
    next_ = ++it;
    doc_.value = prev_->id();
    pos_.clear();

    return doc_.value;
  }

 private:
  class pos_iterator final : public irs::position {
   public:
    pos_iterator(const doc_iterator& owner, irs::IndexFeatures features)
      : owner_(owner) {
      if (irs::IndexFeatures::NONE != (features & irs::IndexFeatures::OFFS)) {
        poffs_ = &offs_;
      }

      if (irs::IndexFeatures::NONE != (features & irs::IndexFeatures::PAY)) {
        ppay_ = &pay_;
      }
    }

    attribute* get_mutable(irs::type_info::type_id type) noexcept override {
      if (irs::type<irs::offset>::id() == type) {
        return poffs_;
      }

      if (irs::type<irs::payload>::id() == type) {
        return ppay_;
      }

      return nullptr;
    }

    void clear() {
      next_ = owner_.prev_->positions().begin();
      value_ = irs::type_limits<irs::type_t::pos_t>::invalid();
      offs_.clear();
      pay_.value = irs::bytes_view{};
    }

    bool next() override {
      if (next_ == owner_.prev_->positions().end()) {
        value_ = irs::type_limits<irs::type_t::pos_t>::eof();
        return false;
      }

      value_ = next_->pos;
      offs_.start = next_->start;
      offs_.end = next_->end;
      pay_.value = next_->payload;
      ++next_;

      return true;
    }

    virtual void reset() override {
      ASSERT_TRUE(false);  // unsupported
    }

   private:
    std::set<posting::position>::const_iterator next_;
    irs::offset offs_;
    irs::payload pay_;
    irs::offset* poffs_{};
    irs::payload* ppay_{};
    const doc_iterator& owner_;
  };

  const tests::term& data_;
  std::map<irs::type_info::type_id, irs::attribute*> attrs_;
  irs::document doc_;
  irs::frequency freq_;
  irs::cost cost_;
  irs::score score_;
  pos_iterator pos_;
  std::set<posting>::const_iterator prev_;
  std::set<posting>::const_iterator next_;
};

doc_iterator::doc_iterator(irs::IndexFeatures features, const tests::term& data)
  : data_(data), pos_(*this, features) {
  next_ = data_.postings.begin();

  cost_.reset(data_.postings.size());
  attrs_[irs::type<irs::cost>::id()] = &cost_;

  attrs_[irs::type<irs::document>::id()] = &doc_;
  attrs_[irs::type<irs::score>::id()] = &score_;

  if (irs::IndexFeatures::NONE != (features & irs::IndexFeatures::FREQ)) {
    attrs_[irs::type<irs::frequency>::id()] = &freq_;
  }

  if (irs::IndexFeatures::NONE != (features & irs::IndexFeatures::POS)) {
    attrs_[irs::type<irs::position>::id()] = &pos_;
  }
}

class term_iterator final : public irs::seek_term_iterator {
 public:
  struct term_cookie final : irs::seek_cookie {
    explicit term_cookie(irs::bytes_view term) noexcept : term(term) {}

    irs::attribute* get_mutable(irs::type_info::type_id) override {
      return nullptr;
    }

    bool IsEqual(const irs::seek_cookie& rhs) const noexcept override {
      return term == irs::down_cast<term_cookie>(rhs).term;
    }

    size_t Hash() const noexcept override {
      return std::hash<irs::bytes_view>{}(term);
    }

    irs::bytes_view term;
  };

  explicit term_iterator(const tests::field& data) noexcept : data_(data) {
    next_ = data_.terms.begin();
  }

  irs::attribute* get_mutable(irs::type_info::type_id type) noexcept override {
    return type == irs::type<irs::term_attribute>::id() ? &value_ : nullptr;
    ;
  }

  irs::bytes_view value() const override { return value_.value; }

  bool next() override {
    if (next_ == data_.terms.end()) {
      value_.value = {};
      return false;
    }

    prev_ = next_, ++next_;
    value_.value = prev_->value;
    return true;
  }

  virtual void read() override {}

  virtual bool seek(irs::bytes_view value) override {
    auto it = data_.terms.find(term{value});

    if (it == data_.terms.end()) {
      prev_ = next_ = it;
      value_.value = {};
      return false;
    }

    prev_ = it;
    next_ = ++it;
    value_.value = prev_->value;
    return true;
  }

  virtual irs::SeekResult seek_ge(irs::bytes_view value) override {
    auto it = data_.terms.lower_bound(term{value});
    if (it == data_.terms.end()) {
      prev_ = next_ = it;
      value_.value = irs::bytes_view{};
      return irs::SeekResult::END;
    }

    if (it->value == value) {
      prev_ = it;
      next_ = ++it;
      value_.value = prev_->value;
      return irs::SeekResult::FOUND;
    }

    prev_ = ++it;
    next_ = ++it;
    value_.value = prev_->value;
    return irs::SeekResult::NOT_FOUND;
  }

  virtual doc_iterator::ptr postings(
    irs::IndexFeatures features) const override {
    return irs::memory::make_managed<doc_iterator>(
      data_.index_features & features, *prev_);
  }

  virtual irs::seek_cookie::ptr cookie() const override {
    return std::make_unique<term_cookie>(value_.value);
  }

 private:
  const tests::field& data_;
  std::set<tests::term>::const_iterator prev_;
  std::set<tests::term>::const_iterator next_;
  irs::term_attribute value_;
};

irs::seek_term_iterator::ptr field::iterator() const {
  return irs::memory::make_managed<term_iterator>(*this);
}

template<typename IteratorFactory>
void assert_docs(irs::doc_iterator::ptr expected_docs,
                 IteratorFactory&& factory) {
  ASSERT_NE(nullptr, expected_docs);

  auto seek_docs = factory();
  ASSERT_NE(nullptr, seek_docs);

  auto seq_docs = factory();
  ASSERT_NE(nullptr, seq_docs);

  ASSERT_TRUE(!irs::doc_limits::valid(expected_docs->value()));
  ASSERT_TRUE(!irs::doc_limits::valid(seq_docs->value()));
  ASSERT_TRUE(!irs::doc_limits::valid(seek_docs->value()));

  while (expected_docs->next()) {
    const auto expected_doc = expected_docs->value();

    ASSERT_TRUE(seq_docs->next());
    ASSERT_EQ(expected_doc, seq_docs->value());

    ASSERT_EQ(expected_doc, seek_docs->seek(expected_doc));
    ASSERT_EQ(expected_doc, seek_docs->value());

    // check document attributes
    {
      auto* expected_freq = irs::get<irs::frequency>(*expected_docs);
      auto* actual_freq = irs::get<irs::frequency>(*seq_docs);

      if (expected_freq) {
        ASSERT_FALSE(!actual_freq);
        ASSERT_EQ(expected_freq->value, actual_freq->value);
      }

      auto* expected_pos = irs::get_mutable<irs::position>(expected_docs.get());
      auto* actual_pos = irs::get_mutable<irs::position>(seq_docs.get());

      if (expected_pos) {
        ASSERT_FALSE(!actual_pos);

        auto* expected_offs = irs::get<irs::offset>(*expected_pos);
        auto* actual_offs = irs::get<irs::offset>(*actual_pos);
        if (expected_offs) ASSERT_FALSE(!actual_offs);

        auto* expected_pay = irs::get<irs::payload>(*expected_pos);
        auto* actual_pay = irs::get<irs::payload>(*actual_pos);
        if (expected_pay) ASSERT_FALSE(!actual_pay);
        ASSERT_TRUE(!irs::pos_limits::valid(expected_pos->value()));
        ASSERT_TRUE(!irs::pos_limits::valid(actual_pos->value()));
        for (; expected_pos->next();) {
          ASSERT_TRUE(actual_pos->next());
          ASSERT_EQ(expected_pos->value(), actual_pos->value());

          if (expected_offs) {
            ASSERT_EQ(expected_offs->start, actual_offs->start);
            ASSERT_EQ(expected_offs->end, actual_offs->end);
          }

          if (expected_pay) {
            ASSERT_EQ(expected_pay->value, actual_pay->value);
          }
        }
        ASSERT_FALSE(actual_pos->next());
        ASSERT_TRUE(irs::pos_limits::eof(expected_pos->value()));
        ASSERT_TRUE(irs::pos_limits::eof(actual_pos->value()));
      }
    }
  }

  ASSERT_TRUE(irs::doc_limits::eof(expected_docs->value()));
  ASSERT_FALSE(seq_docs->next());
  ASSERT_TRUE(irs::doc_limits::eof(seq_docs->value()));
  ASSERT_FALSE(seek_docs->next());
  ASSERT_TRUE(irs::doc_limits::eof(seek_docs->value()));
}

void assert_docs(const irs::term_iterator& expected_term,
                 const irs::term_reader& actual_terms,
                 irs::seek_cookie::ptr actual_cookie,
                 irs::IndexFeatures requested_features) {
  assert_docs(expected_term.postings(requested_features), [&]() {
    return actual_terms.postings(*actual_cookie, requested_features);
  });

  assert_docs(expected_term.postings(requested_features), [&]() {
    return actual_terms.wanderator(*actual_cookie, requested_features);
  });

  // FIXME(gnusi): check bit_union
}

void assert_term(const irs::term_iterator& expected_term,
                 const irs::term_iterator& actual_term,
                 irs::IndexFeatures requested_features) {
  ASSERT_EQ(expected_term.value(), actual_term.value());

  assert_docs(expected_term.postings(requested_features),
              [&]() { return actual_term.postings(requested_features); });
}

void assert_terms_next(const field& expected_field,
                       const irs::term_reader& actual_field,
                       irs::IndexFeatures features,
                       irs::automaton_table_matcher* matcher) {
  irs::bytes_view actual_min{};
  irs::bytes_view actual_max{};
  irs::bstring actual_min_buf;
  irs::bstring actual_max_buf;
  size_t actual_size = 0;

  auto expected_term = expected_field.iterator();
  if (matcher) {
    expected_term = irs::memory::make_managed<irs::automaton_term_iterator>(
      matcher->GetFst(), std::move(expected_term));
  }

  auto actual_term = matcher ? actual_field.iterator(*matcher)
                             : actual_field.iterator(irs::SeekMode::NORMAL);

  for (; expected_term->next(); ++actual_size) {
    ASSERT_TRUE(actual_term->next());

    assert_term(*expected_term, *actual_term, features);
    assert_docs(*expected_term, actual_field, actual_term->cookie(), features);

    if (irs::IsNull(actual_min)) {
      actual_min_buf = actual_term->value();
      actual_min = actual_min_buf;
    }

    actual_max_buf = actual_term->value();
    actual_max = actual_max_buf;
  }
  // FIXME(@gnusi): currently `seek_term_iterator` crashes
  //                if next() is called after iterator is exhausted
  // ASSERT_FALSE(actual_term->next());
  // ASSERT_FALSE(actual_term->next());

  // check term reader
  if (!matcher) {
    ASSERT_EQ(expected_field.terms.size(), actual_size);
    ASSERT_EQ((expected_field.min)(), actual_min);
    ASSERT_EQ((expected_field.max)(), actual_max);
  }
}

void assert_terms_seek(const field& expected_field,
                       const irs::term_reader& actual_field,
                       irs::IndexFeatures features,
                       irs::automaton_table_matcher* matcher,
                       size_t lookahead = 10) {
  auto expected_term = expected_field.iterator();
  if (matcher) {
    expected_term = irs::memory::make_managed<irs::automaton_term_iterator>(
      matcher->GetFst(), std::move(expected_term));
  }

  auto actual_term_with_state =
    matcher ? actual_field.iterator(*matcher)
            : actual_field.iterator(irs::SeekMode::NORMAL);
  ASSERT_NE(nullptr, actual_term_with_state);

  auto actual_term_with_state_random_only =
    actual_field.iterator(irs::SeekMode::RANDOM_ONLY);
  ASSERT_NE(nullptr, actual_term_with_state_random_only);

  for (; expected_term->next();) {
    // seek with state
    {
      ASSERT_TRUE(actual_term_with_state->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term_with_state, features);
    }

    // seek without state random only
    {
      auto actual_term = actual_field.iterator(irs::SeekMode::RANDOM_ONLY);
      ASSERT_TRUE(actual_term->seek(expected_term->value()));

      assert_term(*expected_term, *actual_term, features);
    }

    // seek with state random only
    {
      ASSERT_TRUE(
        actual_term_with_state_random_only->seek(expected_term->value()));

      assert_term(*expected_term, *actual_term_with_state_random_only,
                  features);
    }

    // seek without state, iterate forward
    irs::seek_cookie::ptr cookie;
    {
      auto actual_term = actual_field.iterator(irs::SeekMode::NORMAL);
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
      actual_term->read();
      cookie = actual_term->cookie();

      // iterate forward
      {
        auto copy_expected_term =
          irs::memory::make_managed<term_iterator>(expected_field);

        ASSERT_TRUE(copy_expected_term->seek(expected_term->value()));
        ASSERT_EQ(expected_term->value(), copy_expected_term->value());
        for (size_t i = 0; i < lookahead; ++i) {
          const bool copy_expected_next = copy_expected_term->next();
          const bool actual_next = actual_term->next();
          ASSERT_EQ(copy_expected_next, actual_next);
          if (!copy_expected_next) {
            break;
          }
          assert_term(*copy_expected_term, *actual_term, features);
        }
      }

      // seek back to initial term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
    }

    // seek greater or equal without state, iterate forward
    {
      auto actual_term = actual_field.iterator(irs::SeekMode::NORMAL);
      ASSERT_EQ(irs::SeekResult::FOUND,
                actual_term->seek_ge(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);

      // iterate forward
      {
        auto copy_expected_term =
          irs::memory::make_managed<term_iterator>(expected_field);
        ASSERT_TRUE(copy_expected_term->seek(expected_term->value()));
        ASSERT_EQ(expected_term->value(), copy_expected_term->value());
        for (size_t i = 0; i < lookahead; ++i) {
          const bool copy_expected_next = copy_expected_term->next();
          const bool actual_next = actual_term->next();
          ASSERT_EQ(copy_expected_next, actual_next);
          if (!copy_expected_next) {
            break;
          }
          assert_term(*copy_expected_term, *actual_term, features);
        }
      }

      // seek back to initial term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
    }

    // seek to cookie without state, iterate to the end
    {
      auto actual_term = actual_field.iterator(irs::SeekMode::NORMAL);

      // seek to the same term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);

      // seek to the same term
      ASSERT_TRUE(actual_term->seek(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);

      // seek greater equal to the same term
      ASSERT_EQ(irs::SeekResult::FOUND,
                actual_term->seek_ge(expected_term->value()));
      assert_term(*expected_term, *actual_term, features);
    }
  }
}

void assert_pk(
  const irs::column_reader& actual_reader,
  const std::vector<std::pair<irs::bstring, irs::doc_id_t>>& expected_values) {
  ASSERT_EQ(expected_values.size(), actual_reader.size());

  // check iterators & values
  {
    auto actual_it = actual_reader.iterator(irs::ColumnHint::kNormal);
    ASSERT_NE(nullptr, actual_it);

    auto actual_seek_it = actual_reader.iterator(irs::ColumnHint::kNormal);
    ASSERT_NE(nullptr, actual_seek_it);

    auto* actual_key = irs::get<irs::document>(*actual_it);
    ASSERT_NE(nullptr, actual_key);
    auto* actual_value = irs::get<irs::payload>(*actual_it);
    ASSERT_NE(nullptr, actual_value);

    irs::doc_id_t expected_key = irs::doc_limits::min();
    for (auto& expected : expected_values) {
      auto& expected_value = expected.first;
      ASSERT_TRUE(actual_it->next());

      auto actual_stateless_seek_it =
        actual_reader.iterator(irs::ColumnHint::kNormal);
      ASSERT_NE(nullptr, actual_stateless_seek_it);

      ASSERT_EQ(expected_key, actual_it->value());
      ASSERT_EQ(expected_key, actual_key->value);
      ASSERT_EQ(expected_value, actual_value->value);
      ASSERT_EQ(expected_key, actual_seek_it->seek(expected_key));
      ASSERT_EQ(expected_key, actual_stateless_seek_it->seek(expected_key));

      ++expected_key;
    }
    ASSERT_FALSE(actual_it->next());
    ASSERT_FALSE(actual_it->next());
  }

  // check visit
  {
    auto begin = expected_values.begin();
    irs::doc_id_t expected_key = irs::doc_limits::min();

    visit(actual_reader, [&begin, &expected_key](
                           auto actual_key, const auto& actual_value) mutable {
      EXPECT_EQ(expected_key, actual_key);
      EXPECT_EQ(begin->first, actual_value);
      ++begin;
      ++expected_key;
      return true;
    });
    ASSERT_EQ(begin, expected_values.end());
  }
}

void assert_column(const irs::column_reader* actual_reader,
                   const column_values& expected_values) {
  if (!actual_reader) {
    ASSERT_TRUE(expected_values.empty());
    return;
  }

  if (irs::IsNull(expected_values.name())) {
    // field features are stored as annonymous columns
    ASSERT_TRUE(irs::IsNull(actual_reader->name()));
  } else {
    ASSERT_EQ(expected_values.name(), actual_reader->name());
  }

  if (!irs::IsNull(actual_reader->payload())) {
    // old formats may not support column header payload
    ASSERT_EQ(expected_values.payload(), actual_reader->payload());
  }

  ASSERT_EQ(expected_values.size(), actual_reader->size());

  // check iterators & values
  {
    auto actual_it = actual_reader->iterator(irs::ColumnHint::kNormal);
    ASSERT_NE(nullptr, actual_it);

    auto actual_seek_it = actual_reader->iterator(irs::ColumnHint::kNormal);
    ASSERT_NE(nullptr, actual_seek_it);

    auto* actual_key = irs::get<irs::document>(*actual_it);
    ASSERT_NE(nullptr, actual_key);
    auto* actual_value = irs::get<irs::payload>(*actual_it);
    ASSERT_NE(nullptr, actual_value);

    for (auto& [expected_key, expected_value] : expected_values) {
      ASSERT_TRUE(actual_it->next());

      auto actual_stateless_seek_it =
        actual_reader->iterator(irs::ColumnHint::kNormal);
      ASSERT_NE(nullptr, actual_stateless_seek_it);

      ASSERT_EQ(expected_key, actual_it->value());
      ASSERT_EQ(expected_key, actual_key->value);
      ASSERT_EQ(expected_value, actual_value->value);
      ASSERT_EQ(expected_key, actual_seek_it->seek(expected_key));
      ASSERT_EQ(expected_key, actual_stateless_seek_it->seek(expected_key));
    }
    ASSERT_FALSE(actual_it->next());
    ASSERT_FALSE(actual_it->next());
  }

  // check visit
  {
    auto begin = expected_values.begin();

    visit(*actual_reader,
          [&begin](auto actual_key, const auto& actual_value) mutable {
            auto& [expected_key, expected_value] = *begin;
            EXPECT_EQ(expected_key, actual_key);
            EXPECT_EQ(expected_value, actual_value);
            ++begin;
            return true;
          });
    ASSERT_EQ(begin, expected_values.end());
  }
}

void assert_columnstore(irs::index_reader::ptr actual_index,
                        const index_t& expected_index, size_t skip /*= 0*/) {
  // check number of segments
  ASSERT_EQ(expected_index.size(), actual_index->size());
  size_t i = 0;
  for (auto& actual_segment : *actual_index) {
    // skip segment if validation not required
    if (skip) {
      ++i;
      --skip;
      continue;
    }

    const tests::index_segment& expected_segment = expected_index[i];

    // check pk if present
    if (auto& expected_pk = expected_segment.pk(); !expected_pk.empty()) {
      auto* actual_pk = actual_segment.sort();
      ASSERT_NE(nullptr, actual_pk);
      assert_pk(*actual_pk, expected_pk);
    }

    // check stored columns
    auto& expected_columns = expected_segment.named_columns();
    auto expected_columns_begin = expected_columns.begin();
    auto actual_columns = actual_segment.columns();

    for (; actual_columns->next(); ++expected_columns_begin) {
      auto& actual_column = actual_columns->value();
      ASSERT_EQ(expected_columns_begin->first, actual_column.name());
      // column id is format dependent
      ASSERT_TRUE(
        irs::field_limits::valid(expected_columns_begin->second->id()));
      ASSERT_TRUE(irs::field_limits::valid(actual_column.id()));
      ASSERT_LT(expected_columns_begin->second->id(),
                expected_segment.columns().size());

      const auto* actual_column_reader =
        actual_segment.column(actual_column.id());
      ASSERT_EQ(actual_column_reader,
                actual_segment.column(actual_column.name()));

      assert_column(
        actual_column_reader,
        expected_segment.columns()[expected_columns_begin->second->id()]);
    }
    ASSERT_FALSE(actual_columns->next());
    ASSERT_EQ(expected_columns_begin, expected_columns.end());

    // check stored features
    auto& expected_fields = expected_segment.fields();
    auto expected_field = expected_fields.begin();
    auto actual_fields = actual_segment.fields();
    for (; actual_fields->next(); ++expected_field) {
      auto actual_field_feature =
        actual_fields->value().meta().features.begin();
      for (auto& expected_field_feature : expected_field->second.features) {
        ASSERT_EQ(expected_field_feature.first, actual_field_feature->first);
        if (!irs::field_limits::valid(expected_field_feature.second)) {
          ASSERT_FALSE(irs::field_limits::valid(actual_field_feature->second));
        } else {
          ASSERT_LT(expected_field_feature.second,
                    expected_segment.columns().size());
          const auto* actual_column =
            actual_segment.column(actual_field_feature->second);
          assert_column(
            actual_column,
            expected_segment.columns()[expected_field_feature.second]);
        }
        ++actual_field_feature;
      }
      ASSERT_EQ(actual_field_feature,
                actual_fields->value().meta().features.end());
    }
    ASSERT_FALSE(actual_fields->next());
    ASSERT_EQ(expected_field, expected_fields.end());

    ++i;
  }
}

void assert_columnstore(const irs::directory& dir, irs::format::ptr codec,
                        const index_t& expected_index, size_t skip /*= 0*/) {
  auto reader = irs::directory_reader::open(dir, codec);
  ASSERT_NE(nullptr, reader);

  assert_columnstore(static_cast<irs::index_reader::ptr>(reader),
                     expected_index, skip);
}

void assert_index(irs::index_reader::ptr actual_index,
                  const index_t& expected_index, irs::IndexFeatures features,
                  size_t skip /*= 0*/,
                  irs::automaton_table_matcher* matcher /*=nullptr*/) {
  // check number of segments
  ASSERT_EQ(expected_index.size(), actual_index->size());
  size_t i = 0;
  for (auto& actual_segment : *actual_index) {
    // skip segment if validation not required
    if (skip) {
      ++i;
      --skip;
      continue;
    }

    const tests::index_segment& expected_segment = expected_index[i];

    // segment normally returns a reference to itself
    ASSERT_EQ(1, actual_segment.size());
    ASSERT_EQ(&actual_segment, &*actual_segment.begin());

    // get field name iterators
    auto& expected_fields = expected_segment.fields();
    auto expected_field = expected_fields.begin();

    // iterate over fields
    auto actual_fields = actual_segment.fields();
    for (; actual_fields->next(); ++expected_field) {
      ASSERT_EQ(expected_field->first, actual_fields->value().meta().name);
      ASSERT_EQ(expected_field->second.name,
                actual_fields->value().meta().name);
      ASSERT_EQ(expected_field->second.index_features,
                actual_fields->value().meta().index_features);

      // check field terms
      const auto* actual_terms = actual_segment.field(expected_field->first);
      ASSERT_NE(nullptr, actual_terms);
      ASSERT_EQ(actual_fields->value().meta(), actual_terms->meta());

      // check term reader
      ASSERT_EQ((expected_field->second.min)(), (actual_terms->min)());
      ASSERT_EQ((expected_field->second.max)(), (actual_terms->max)());
      ASSERT_EQ(expected_field->second.terms.size(), actual_terms->size());
      ASSERT_EQ(expected_field->second.docs.size(), actual_terms->docs_count());

      // check field meta
      const irs::field_meta& expected_meta = expected_field->second;
      const irs::field_meta& actual_meta = actual_terms->meta();
      ASSERT_EQ(expected_meta.name, actual_meta.name);
      ASSERT_EQ(expected_meta.index_features, actual_meta.index_features);
      {
        auto& expected_features = expected_meta.features;
        auto actual_features = actual_meta.features;
        ASSERT_EQ(expected_features.size(), actual_features.size());
        for (auto& entry : expected_features) {
          ASSERT_EQ(1, actual_features.erase(entry.first));
          // we don't check column ids as they are format dependent
        }
        ASSERT_TRUE(actual_features.empty());
      }

      auto* actual_freq = irs::get<irs::frequency>(*actual_terms);
      if (irs::IndexFeatures::NONE !=
          (expected_field->second.index_features & irs::IndexFeatures::FREQ)) {
        ASSERT_NE(nullptr, actual_freq);
        ASSERT_EQ(expected_field->second.total_freq(), actual_freq->value);
      } else {
        ASSERT_EQ(nullptr, actual_freq);
      }

      assert_terms_next(expected_field->second, *actual_terms, features,
                        matcher);
      assert_terms_seek(expected_field->second, *actual_terms, features,
                        matcher);
    }
    ASSERT_FALSE(actual_fields->next());

    ++i;
    ASSERT_EQ(expected_fields.end(), expected_field);
  }
}

void assert_index(const irs::directory& dir, irs::format::ptr codec,
                  const index_t& expected_index, irs::IndexFeatures features,
                  size_t skip /*= 0*/,
                  irs::automaton_table_matcher* matcher /*= nullptr*/) {
  auto reader = irs::directory_reader::open(dir, codec);
  ASSERT_NE(nullptr, reader);

  assert_index(static_cast<irs::index_reader::ptr>(reader), expected_index,
               features, skip, matcher);
}

}  // namespace tests

namespace iresearch {

// use base irs::position type for ancestors
template<>
struct type<tests::doc_iterator::pos_iterator> : type<irs::position> {};

}  // namespace iresearch
