﻿////////////////////////////////////////////////////////////////////////////////
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

#include "merge_writer.hpp"

#include <absl/container/flat_hash_map.h>

#include <array>
#include <boost/iterator/filter_iterator.hpp>
#include <deque>

#include "analysis/token_attributes.hpp"
#include "index/comparer.hpp"
#include "index/field_meta.hpp"
#include "index/heap_iterator.hpp"
#include "index/index_meta.hpp"
#include "index/norm.hpp"
#include "index/segment_reader.hpp"
#include "store/store_utils.hpp"
#include "utils/directory_utils.hpp"
#include "utils/log.hpp"
#include "utils/lz4compression.hpp"
#include "utils/memory.hpp"
#include "utils/timer_utils.hpp"
#include "utils/type_limits.hpp"
#include "utils/version_utils.hpp"

namespace {

using namespace irs;

bool is_subset_of(const feature_map_t& lhs, const feature_map_t& rhs) noexcept {
  for (auto& entry : lhs) {
    if (!rhs.count(entry.first)) {
      return false;
    }
  }
  return true;
}

void accumulate_features(feature_set_t& accum, const feature_map_t& features) {
  for (auto& entry : features) {
    accum.emplace(entry.first);
  }
}

// mapping of old doc_id to new doc_id (reader doc_ids are sequential 0 based)
// masked doc_ids have value of MASKED_DOC_ID
using doc_id_map_t = std::vector<doc_id_t>;

// document mapping function
using doc_map_f = std::function<doc_id_t(doc_id_t)>;

using field_meta_map_t =
  absl::flat_hash_map<std::string_view, const field_meta*>;

class noop_directory : public directory {
 public:
  static noop_directory& instance() {
    static noop_directory INSTANCE;
    return INSTANCE;
  }

  virtual directory_attributes& attributes() noexcept override {
    return attrs_;
  }

  virtual index_output::ptr create(std::string_view) noexcept override {
    return nullptr;
  }

  virtual bool exists(bool&, std::string_view) const noexcept override {
    return false;
  }

  virtual bool length(uint64_t&, std::string_view) const noexcept override {
    return false;
  }

  virtual index_lock::ptr make_lock(std::string_view) noexcept override {
    return nullptr;
  }

  virtual bool mtime(std::time_t&, std::string_view) const noexcept override {
    return false;
  }

  virtual index_input::ptr open(std::string_view,
                                IOAdvice) const noexcept override {
    return nullptr;
  }

  virtual bool remove(std::string_view) noexcept override { return false; }

  virtual bool rename(std::string_view, std::string_view) noexcept override {
    return false;
  }

  virtual bool sync(std::string_view) noexcept override { return false; }

  virtual bool visit(const directory::visitor_f&) const override {
    return false;
  }

 private:
  noop_directory() = default;

  directory_attributes attrs_{0, nullptr};
};  // noop_directory

class progress_tracker {
 public:
  explicit progress_tracker(const merge_writer::flush_progress_t& progress,
                            size_t count) noexcept
    : progress_(&progress), count_(count) {
    assert(progress);
  }

  bool operator()() {
    if (hits_++ >= count_) {
      hits_ = 0;
      valid_ = (*progress_)();
    }

    return valid_;
  }

  explicit operator bool() const noexcept { return valid_; }

  void reset() noexcept {
    hits_ = 0;
    valid_ = true;
  }

 private:
  const merge_writer::flush_progress_t* progress_;
  const size_t count_;  // call progress callback each `count_` hits
  size_t hits_{0};      // current number of hits
  bool valid_{true};
};  // progress_tracker

class remapping_doc_iterator final : public doc_iterator {
 public:
  remapping_doc_iterator(doc_iterator::ptr&& it,
                         const doc_map_f& mapper) noexcept
    : it_{std::move(it)}, mapper_{&mapper}, src_{irs::get<document>(*it_)} {
    assert(it_ && src_);
  }

  bool next() override;

  virtual doc_id_t value() const noexcept override { return doc_.value; }

  virtual doc_id_t seek(doc_id_t target) override {
    irs::seek(*this, target);
    return value();
  }

  virtual attribute* get_mutable(
    irs::type_info::type_id type) noexcept override {
    return irs::type<irs::document>::id() == type ? &doc_
                                                  : it_->get_mutable(type);
  }

 private:
  doc_iterator::ptr it_;
  const doc_map_f* mapper_;
  const irs::document* src_;
  irs::document doc_;
};

bool remapping_doc_iterator::next() {
  while (it_->next()) {
    doc_.value = (*mapper_)(src_->value);

    if (doc_limits::eof(doc_.value)) {
      continue;  // masked doc_id
    }

    return true;
  }

  return false;
}

// Iterator over doc_ids for a term over all readers
class compound_doc_iterator : public doc_iterator {
 public:
  typedef std::pair<doc_iterator::ptr, std::reference_wrapper<const doc_map_f>>
    doc_iterator_t;
  typedef std::vector<doc_iterator_t> iterators_t;

  static constexpr const size_t kProgressStepDocs = size_t(1) << 14;

  explicit compound_doc_iterator(
    const merge_writer::flush_progress_t& progress) noexcept
    : progress_(progress, kProgressStepDocs) {}

  template<typename Func>
  bool reset(Func&& func) {
    if (!func(iterators_)) {
      return false;
    }

    doc_.value = doc_limits::invalid();
    current_itr_ = 0;

    return true;
  }

  size_t size() const noexcept { return iterators_.size(); }

  bool aborted() const noexcept { return !static_cast<bool>(progress_); }

  virtual attribute* get_mutable(
    irs::type_info::type_id type) noexcept override final {
    if (irs::type<irs::document>::id() == type) {
      return &doc_;
    }

    return irs::type<attribute_provider_change>::id() == type
             ? &attribute_change_
             : nullptr;
  }

  virtual bool next() override;

  virtual doc_id_t seek(doc_id_t target) override final {
    irs::seek(*this, target);
    return value();
  }

  virtual doc_id_t value() const noexcept override final { return doc_.value; }

 private:
  friend class sorting_compound_doc_iterator;

  attribute_provider_change attribute_change_;
  std::vector<doc_iterator_t> iterators_;
  size_t current_itr_{0};
  progress_tracker progress_;
  document doc_;
};  // compound_doc_iterator

bool compound_doc_iterator::next() {
  progress_();

  if (aborted()) {
    doc_.value = doc_limits::eof();
    iterators_.clear();
    return false;
  }

  for (bool notify = !doc_limits::valid(doc_.value);
       current_itr_ < iterators_.size(); notify = true, ++current_itr_) {
    auto& itr_entry = iterators_[current_itr_];
    auto& itr = itr_entry.first;
    auto& id_map = itr_entry.second.get();

    if (!itr) {
      continue;
    }

    if (notify) {
      attribute_change_(*itr);
    }

    while (itr->next()) {
      doc_.value = id_map(itr->value());

      if (doc_limits::eof(doc_.value)) {
        continue;  // masked doc_id
      }

      return true;
    }

    itr.reset();
  }

  doc_.value = doc_limits::eof();

  return false;
}

// Iterator over sorted doc_ids for a term over all readers
class sorting_compound_doc_iterator final : public doc_iterator {
 public:
  explicit sorting_compound_doc_iterator(compound_doc_iterator& doc_it) noexcept
    : doc_it_{&doc_it}, heap_it_{min_heap_context{doc_it.iterators_}} {}

  template<typename Func>
  bool reset(Func&& func) {
    if (!doc_it_->reset(std::forward<Func>(func))) {
      return false;
    }

    heap_it_.reset(doc_it_->iterators_.size());
    lead_ = nullptr;

    return true;
  }

  virtual attribute* get_mutable(
    irs::type_info::type_id type) noexcept override {
    return doc_it_->get_mutable(type);
  }

  virtual bool next() override;

  virtual doc_id_t seek(doc_id_t target) override {
    irs::seek(*this, target);
    return value();
  }

  virtual doc_id_t value() const noexcept override { return doc_it_->value(); }

 private:
  class min_heap_context {
   public:
    explicit min_heap_context(compound_doc_iterator::iterators_t& itrs) noexcept
      : itrs_{&itrs} {}

    // advance
    bool operator()(const size_t i) const {
      assert(i < itrs_->size());
      auto& doc_it = (*itrs_)[i];
      auto const& map = doc_it.second.get();
      while (doc_it.first->next()) {
        if (!doc_limits::eof(map(doc_it.first->value()))) {
          return true;
        }
      }
      return false;
    }

    // compare
    bool operator()(const size_t lhs, const size_t rhs) const {
      return remap(lhs) > remap(rhs);
    }

   private:
    doc_id_t remap(const size_t i) const {
      assert(i < itrs_->size());
      auto& doc_it = (*itrs_)[i];
      return doc_it.second.get()(doc_it.first->value());
    }

    compound_doc_iterator::iterators_t* itrs_;
  };  // min_heap_context

  compound_doc_iterator* doc_it_;
  ExternalHeapIterator<min_heap_context> heap_it_;
  compound_doc_iterator::doc_iterator_t* lead_{};
};  // sorting_compound_doc_iterator

bool sorting_compound_doc_iterator::next() {
  auto& iterators = doc_it_->iterators_;
  auto& current_id = doc_it_->doc_;

  doc_it_->progress_();

  if (doc_it_->aborted()) {
    current_id.value = doc_limits::eof();
    iterators.clear();
    return false;
  }

  while (heap_it_.next()) {
    auto& new_lead = iterators[heap_it_.value()];
    auto& it = new_lead.first;
    auto& doc_map = new_lead.second.get();

    if (&new_lead != lead_) {
      // update attributes
      doc_it_->attribute_change_(*it);
      lead_ = &new_lead;
    }

    current_id.value = doc_map(it->value());

    if (doc_limits::eof(current_id.value)) {
      continue;
    }

    return true;
  }

  current_id.value = doc_limits::eof();

  return false;
}

class doc_iterator_container {
 public:
  explicit doc_iterator_container(size_t size) { itrs_.reserve(size); }

  auto begin() { return std::begin(itrs_); }
  auto end() { return std::end(itrs_); }

  template<typename Func>
  bool reset(Func&& func) {
    return func(itrs_);
  }

 private:
  std::vector<remapping_doc_iterator> itrs_;
};

class compound_column_iterator final {
 public:
  explicit compound_column_iterator(size_t size) {
    iterators_.reserve(size);
    iterator_mask_.reserve(size);
  }

  size_t size() const { return iterators_.size(); }

  void add(const sub_reader& reader, const doc_map_f& doc_map) {
    auto it = reader.columns();
    assert(it);

    if (IRS_LIKELY(it)) {
      iterator_mask_.emplace_back(iterators_.size());
      iterators_.emplace_back(std::move(it), reader, doc_map);
    }
  }

  // visit matched iterators
  template<typename Visitor>
  bool visit(const Visitor& visitor) const {
    for (auto id : iterator_mask_) {
      auto& it = iterators_[id];
      if (!visitor(*it.reader, *it.doc_map, it.it->value())) {
        return false;
      }
    }
    return true;
  }

  const column_reader& value() const {
    if (IRS_LIKELY(current_value_)) {
      return *current_value_;
    }

    return column_iterator::empty()->value();
  }

  bool next() {
    // advance all used iterators
    for (auto id : iterator_mask_) {
      auto& it = iterators_[id].it;

      if (it) {
        // Skip annonymous columns
        bool exhausted;
        do {
          exhausted = !it->next();
        } while (!exhausted && IsNull(it->value().name()));

        if (exhausted) {
          it = nullptr;
        }
      }
    }

    iterator_mask_.clear();  // reset for next pass

    for (size_t i = 0, size = iterators_.size(); i < size; ++i) {
      auto& it = iterators_[i].it;
      if (!it) {
        continue;  // empty iterator
      }

      const auto& value = it->value();
      const std::string_view key = value.name();
      assert(!IsNull(key));

      if (!iterator_mask_.empty() && current_key_ < key) {
        continue;  // empty field or value too large
      }

      // found a smaller field
      if (iterator_mask_.empty() || key < current_key_) {
        iterator_mask_.clear();
        current_key_ = key;
        current_value_ = &value;
      }

      assert(value.name() == current_value_->name());  // validated by caller
      iterator_mask_.push_back(i);
    }

    if (!iterator_mask_.empty()) {
      return true;
    }

    current_key_ = {};

    return false;
  }

 private:
  struct iterator_t : util::noncopyable {
    iterator_t(column_iterator::ptr&& it, const sub_reader& reader,
               const doc_map_f& doc_map)
      : it(std::move(it)), reader(&reader), doc_map(&doc_map) {}

    iterator_t(iterator_t&&) = default;
    iterator_t& operator=(iterator_t&&) = delete;

    column_iterator::ptr it;
    const sub_reader* reader;
    const doc_map_f* doc_map;
  };

  static_assert(std::is_nothrow_move_constructible_v<iterator_t>);

  const column_reader* current_value_{};
  std::string_view current_key_;
  std::vector<size_t> iterator_mask_;  // valid iterators for current step
  std::vector<iterator_t> iterators_;  // all segment iterators
};

// Iterator over documents for a term over all readers
class compound_term_iterator final : public term_iterator {
 public:
  static constexpr const size_t kProgressStepTerms = size_t(1) << 7;

  explicit compound_term_iterator(
    const merge_writer::flush_progress_t& progress, const comparer* comparator)
    : doc_itr_(progress),
      psorting_doc_itr_(nullptr == comparator ? nullptr : &sorting_doc_itr_),
      progress_(progress, kProgressStepTerms) {}

  bool aborted() const {
    return !static_cast<bool>(progress_) || doc_itr_.aborted();
  }

  void reset(const field_meta& meta) noexcept {
    meta_ = &meta;
    term_iterator_mask_.clear();
    term_iterators_.clear();
    current_term_ = {};
  }

  const field_meta& meta() const noexcept { return *meta_; }
  void add(const term_reader& reader, const doc_map_f& doc_map);
  virtual attribute* get_mutable(irs::type_info::type_id) noexcept override {
    // no way to merge attributes for the same term spread over multiple
    // iterators would require API change for attributes
    assert(false);
    return nullptr;
  }
  virtual bool next() override;
  virtual doc_iterator::ptr postings(IndexFeatures features) const override;
  virtual void read() override {
    for (auto& itr_id : term_iterator_mask_) {
      if (term_iterators_[itr_id].first) {
        term_iterators_[itr_id].first->read();
      }
    }
  }
  virtual bytes_view value() const override { return current_term_; }

 private:
  struct term_iterator_t {
    seek_term_iterator::ptr first;
    const doc_map_f* second;

    term_iterator_t(seek_term_iterator::ptr&& term_itr,
                    const doc_map_f* doc_map)
      : first(std::move(term_itr)), second(doc_map) {}

    term_iterator_t(term_iterator_t&& other) noexcept
      : first(std::move(other.first)), second(std::move(other.second)) {}
  };

  compound_term_iterator(const compound_term_iterator&) =
    delete;  // due to references
  compound_term_iterator& operator=(const compound_term_iterator&) =
    delete;  // due to references

  bytes_view current_term_;
  const field_meta* meta_{};
  std::vector<size_t> term_iterator_mask_;  // valid iterators for current term
  std::vector<term_iterator_t> term_iterators_;  // all term iterators
  mutable compound_doc_iterator doc_itr_;
  mutable sorting_compound_doc_iterator sorting_doc_itr_{doc_itr_};
  sorting_compound_doc_iterator* psorting_doc_itr_;
  progress_tracker progress_;
};  // compound_term_iterator

void compound_term_iterator::add(const term_reader& reader,
                                 const doc_map_f& doc_id_map) {
  auto it = reader.iterator(SeekMode::NORMAL);
  assert(it);

  if (IRS_LIKELY(it)) {
    // mark as used to trigger next()
    term_iterator_mask_.emplace_back(term_iterators_.size());
    term_iterators_.emplace_back(std::move(it), &doc_id_map);
  }
}

bool compound_term_iterator::next() {
  progress_();

  if (aborted()) {
    term_iterators_.clear();
    term_iterator_mask_.clear();
    return false;
  }

  // advance all used iterators
  for (auto& itr_id : term_iterator_mask_) {
    auto& it = term_iterators_[itr_id].first;
    if (it && !it->next()) {
      it.reset();
    }
  }

  term_iterator_mask_.clear();  // reset for next pass

  for (size_t i = 0, count = term_iterators_.size(); i < count; ++i) {
    auto& term_itr = term_iterators_[i];

    if (!term_itr.first || (!term_iterator_mask_.empty() &&
                            term_itr.first->value() > current_term_)) {
      continue;  // empty iterator or value too large
    }

    // found a smaller term
    if (term_iterator_mask_.empty() ||
        term_itr.first->value() < current_term_) {
      term_iterator_mask_.clear();
      current_term_ = term_itr.first->value();
    }

    term_iterator_mask_.emplace_back(i);
  }

  if (!term_iterator_mask_.empty()) {
    return true;
  }

  current_term_ = {};

  return false;
}

doc_iterator::ptr compound_term_iterator::postings(
  IndexFeatures /*features*/) const {
  auto add_iterators = [this](compound_doc_iterator::iterators_t& itrs) {
    itrs.clear();
    itrs.reserve(term_iterator_mask_.size());

    for (auto& itr_id : term_iterator_mask_) {
      auto& term_itr = term_iterators_[itr_id];

      auto it = term_itr.first->postings(meta().index_features);
      assert(it);

      if (IRS_LIKELY(it)) {
        itrs.emplace_back(std::move(it), *term_itr.second);
      }
    }

    return true;
  };

  doc_iterator* doc_itr = &doc_itr_;

  if (psorting_doc_itr_) {
    sorting_doc_itr_.reset(add_iterators);
    if (doc_itr_.size() > 1) {
      doc_itr = psorting_doc_itr_;
    }
  } else {
    doc_itr_.reset(add_iterators);
  }

  return memory::to_managed<doc_iterator, false>(doc_itr);
}

// Iterator over field_ids over all readers
class compound_field_iterator final : public basic_term_reader {
 public:
  static constexpr const size_t kProgressStepFields = size_t(1);

  explicit compound_field_iterator(
    size_t size, const merge_writer::flush_progress_t& progress,
    const comparer* comparator = nullptr)
    : term_itr_(progress, comparator),
      progress_(progress, kProgressStepFields) {
    field_iterators_.reserve(size);
    field_iterator_mask_.reserve(size);
  }

  void add(const sub_reader& reader, const doc_map_f& doc_id_map);
  bool next();
  size_t size() const noexcept { return field_iterators_.size(); }

  // visit matched iterators
  template<typename Visitor>
  bool visit(const Visitor& visitor) const {
    for (auto& entry : field_iterator_mask_) {
      auto& itr = field_iterators_[entry.itr_id];
      if (!visitor(*itr.reader, *itr.doc_map, *entry.meta)) {
        return false;
      }
    }
    return true;
  }

  virtual const field_meta& meta() const noexcept override {
    assert(current_meta_);
    return *current_meta_;
  }

  virtual bytes_view(min)() const noexcept override { return min_; }

  virtual bytes_view(max)() const noexcept override { return max_; }

  virtual attribute* get_mutable(irs::type_info::type_id) noexcept override {
    return nullptr;
  }

  virtual term_iterator::ptr iterator() const override;

  bool aborted() const {
    return !static_cast<bool>(progress_) || term_itr_.aborted();
  }

 private:
  struct field_iterator_t : util::noncopyable {
    field_iterator_t(field_iterator::ptr&& itr, const sub_reader& reader,
                     const doc_map_f& doc_map)
      : itr(std::move(itr)), reader(&reader), doc_map(&doc_map) {}

    field_iterator_t(field_iterator_t&&) = default;
    field_iterator_t& operator=(field_iterator_t&&) = delete;

    field_iterator::ptr itr;
    const sub_reader* reader;
    const doc_map_f* doc_map;
  };

  static_assert(std::is_nothrow_move_constructible_v<field_iterator_t>);

  struct term_iterator_t {
    size_t itr_id;
    const field_meta* meta;
    const term_reader* reader;
  };

  std::string_view current_field_;
  const field_meta* current_meta_{&field_meta::kEmpty};
  bytes_view min_{};
  bytes_view max_{};
  std::vector<term_iterator_t>
    field_iterator_mask_;  // valid iterators for current field
  std::vector<field_iterator_t> field_iterators_;  // all segment iterators
  mutable compound_term_iterator term_itr_;
  progress_tracker progress_;
};  // compound_field_iterator

void compound_field_iterator::add(const sub_reader& reader,
                                  const doc_map_f& doc_id_map) {
  auto it = reader.fields();
  assert(it);

  if (IRS_LIKELY(it)) {
    field_iterator_mask_.emplace_back(
      term_iterator_t{field_iterators_.size(), nullptr,
                      nullptr});  // mark as used to trigger next()
    field_iterators_.emplace_back(std::move(it), reader, doc_id_map);
  }
}

bool compound_field_iterator::next() {
  progress_();

  if (aborted()) {
    field_iterators_.clear();
    field_iterator_mask_.clear();
    current_field_ = {};
    max_ = min_ = {};
    return false;
  }

  // advance all used iterators
  for (auto& entry : field_iterator_mask_) {
    auto& it = field_iterators_[entry.itr_id];
    if (it.itr && !it.itr->next()) {
      it.itr = nullptr;
    }
  }

  // reset for next pass
  field_iterator_mask_.clear();
  max_ = min_ = {};

  for (size_t i = 0, count = field_iterators_.size(); i < count; ++i) {
    auto& field_itr = field_iterators_[i];

    if (!field_itr.itr) {
      continue;  // empty iterator
    }

    const auto& field_meta = field_itr.itr->value().meta();
    const auto* field_terms = field_itr.reader->field(field_meta.name);
    const std::string_view field_id = field_meta.name;

    if (!field_terms ||
        (!field_iterator_mask_.empty() && field_id > current_field_)) {
      continue;  // empty field or value too large
    }

    // found a smaller field
    if (field_iterator_mask_.empty() || field_id < current_field_) {
      field_iterator_mask_.clear();
      current_field_ = field_id;
      current_meta_ = &field_meta;
    }

    // validated by caller
    assert(is_subset_of(field_meta.features, meta().features));
    assert(field_meta.index_features <= meta().index_features);

    field_iterator_mask_.emplace_back(
      term_iterator_t{i, &field_meta, field_terms});

    // update min and max terms
    min_ = std::min(min_, field_terms->min());
    max_ = std::max(max_, field_terms->max());
  }

  if (!field_iterator_mask_.empty()) {
    return true;
  }

  current_field_ = {};

  return false;
}

term_iterator::ptr compound_field_iterator::iterator() const {
  term_itr_.reset(meta());

  for (auto& segment : field_iterator_mask_) {
    term_itr_.add(*(segment.reader),
                  *(field_iterators_[segment.itr_id].doc_map));
  }

  return memory::to_managed<term_iterator, false>(&term_itr_);
}

// Computes fields_type
bool compute_field_meta(field_meta_map_t& field_meta_map,
                        IndexFeatures& index_features,
                        feature_set_t& fields_features,
                        const sub_reader& reader) {
  REGISTER_TIMER_DETAILED();

  for (auto it = reader.fields(); it->next();) {
    const auto& field_meta = it->value().meta();
    const auto [field_meta_it, is_new] =
      field_meta_map.emplace(field_meta.name, &field_meta);

    // validate field_meta equivalence
    if (!is_new &&
        (!is_subset_of(field_meta.index_features,
                       field_meta_it->second->index_features) ||
         !is_subset_of(field_meta.features, field_meta_it->second->features))) {
      return false;  // field_meta is not equal, so cannot merge segments
    }

    accumulate_features(fields_features, field_meta.features);
    index_features |= field_meta.index_features;
  }

  return true;
}

// Helper class responsible for writing a data from different sources
// into single columnstore.
class columnstore {
 public:
  static constexpr size_t kProgressStepColumn = size_t{1} << 13;

  columnstore(columnstore_writer::ptr&& writer,
              const merge_writer::flush_progress_t& progress)
    : progress_{progress, kProgressStepColumn}, writer_{std::move(writer)} {}

  columnstore(directory& dir, const segment_meta& meta,
              const merge_writer::flush_progress_t& progress)
    : progress_{progress, kProgressStepColumn} {
    auto writer = meta.codec->get_columnstore_writer(true);
    writer->prepare(dir, meta);

    writer_ = std::move(writer);
  }

  // Inserts live values from the specified iterators into a column.
  // Returns column id of the inserted column on success,
  //  field_limits::invalid() in case if no data were inserted,
  //  empty value is case if operation was interrupted.
  template<typename Writer>
  std::optional<field_id> insert(
    doc_iterator_container& itrs, const column_info& info,
    columnstore_writer::column_finalizer_f&& finalizer, Writer&& writer);

  // Inserts live values from the specified 'iterator' into a column.
  // Returns column id of the inserted column on success,
  //  field_limits::invalid() in case if no data were inserted,
  //  empty value is case if operation was interrupted.
  template<typename Writer>
  std::optional<field_id> insert(
    sorting_compound_doc_iterator& it, const column_info& info,
    columnstore_writer::column_finalizer_f&& finalizer, Writer&& writer);

  // Returns `true` if anything was actually flushed
  bool flush(const flush_state& state) { return writer_->commit(state); }

  bool valid() const noexcept { return static_cast<bool>(writer_); }

 private:
  progress_tracker progress_;
  columnstore_writer::ptr writer_;
};

template<typename Writer>
std::optional<field_id> columnstore::insert(
  doc_iterator_container& itrs, const column_info& info,
  columnstore_writer::column_finalizer_f&& finalizer, Writer&& writer) {
  auto next_iterator = [end = std::end(itrs)](auto begin) {
    return std::find_if(begin, end, [](auto& it) { return it.next(); });
  };

  auto begin = next_iterator(std::begin(itrs));

  if (begin == std::end(itrs)) {
    // Empty column
    return std::make_optional(field_limits::invalid());
  }

  auto column = writer_->push_column(info, std::move(finalizer));

  auto write_column = [&column, &writer, this](auto& it) -> bool {
    auto* payload = irs::get<irs::payload>(it);

    do {
      if (!progress_()) {
        // Stop was requested
        return false;
      }

      auto& out = column.second(it.value());

      if (payload) {
        writer(out, payload->value);
      }
    } while (it.next());

    return true;
  };

  do {
    if (!write_column(*begin)) {
      // Stop was requested
      return std::nullopt;
    }

    begin = next_iterator(++begin);
  } while (begin != std::end(itrs));

  return std::make_optional(column.first);
}

template<typename Writer>
std::optional<field_id> columnstore::insert(
  sorting_compound_doc_iterator& it, const column_info& info,
  columnstore_writer::column_finalizer_f&& finalizer, Writer&& writer) {
  const payload* payload = nullptr;

  auto* callback = irs::get<attribute_provider_change>(it);

  if (callback) {
    callback->subscribe([&payload](const attribute_provider& attrs) {
      payload = irs::get<irs::payload>(attrs);
    });
  } else {
    payload = irs::get<irs::payload>(it);
  }

  if (it.next()) {
    auto column = writer_->push_column(info, std::move(finalizer));

    do {
      if (!progress_()) {
        // Stop was requested
        return std::nullopt;
      }

      auto& out = column.second(it.value());

      if (payload) {
        writer(out, payload->value);
      }
    } while (it.next());

    return std::make_optional(column.first);
  } else {
    // Empty column
    return std::make_optional(field_limits::invalid());
  }
}

struct SortingIteratorAdapter {
  explicit SortingIteratorAdapter(doc_iterator::ptr it) noexcept
    : it{std::move(it)},
      doc{irs::get<irs::document>(*this->it)},
      payload{irs::get<irs::payload>(*this->it)} {}

  [[nodiscard]] bool valid() const noexcept { return doc && payload; }

  doc_iterator::ptr it;
  const irs::document* doc;
  const irs::payload* payload;
};

class SortingCompoundDocIterator : util::noncopyable {
 public:
  explicit SortingCompoundDocIterator(
    const comparer& comparator, std::vector<SortingIteratorAdapter>&& itrs)
    : itrs_{std::move(itrs)}, heap_it_{MinHeapContext{itrs_, comparator}} {
    heap_it_.reset(itrs_.size());
  }

  bool next() { return heap_it_.next(); }

  std::pair<size_t, const SortingIteratorAdapter*> value() const noexcept {
    const auto idx = heap_it_.value();
    return {idx, &itrs_[idx]};
  }

 private:
  class MinHeapContext {
   public:
    explicit MinHeapContext(std::span<SortingIteratorAdapter> itrs,
                            const comparer& less) noexcept
      : itrs_{itrs}, less_{&less} {}

    // advance
    bool operator()(const size_t i) const {
      assert(i < itrs_.size());
      return itrs_[i].it->next();
    }

    // compare
    bool operator()(const size_t lhs, const size_t rhs) const {
      assert(lhs < itrs_.size());
      assert(rhs < itrs_.size());

      const auto& [lhs_it, lhs_doc, lhs_pay] = itrs_[lhs];
      const auto& [rhs_it, rhs_doc, rhs_pay] = itrs_[rhs];

      // FIXME(gnusi): Consider changing comparator to 3-way comparison
      if (const bytes_view lhs_value = lhs_pay->value,
          rhs_value = rhs_pay->value;
          (*less_)(rhs_value, lhs_value)) {
        return true;
      } else if ((*less_)(lhs_value, rhs_value)) {
        return false;
      }

      // tie braker to avoid splitting document blocks
      return lhs_it < rhs_it ||
             (lhs_it < rhs_it && lhs_doc->value < rhs_doc->value);
    }

   private:
    std::span<SortingIteratorAdapter> itrs_;
    const comparer* less_;
  };

  std::vector<SortingIteratorAdapter> itrs_;
  ExternalHeapIterator<MinHeapContext> heap_it_;
};

template<typename Iterator>
bool write_columns(columnstore& cs, Iterator& columns,
                   const column_info_provider_t& column_info,
                   compound_column_iterator& column_itr,
                   const merge_writer::flush_progress_t& progress) {
  REGISTER_TIMER_DETAILED();
  assert(cs.valid());
  assert(progress);

  auto add_iterators = [&column_itr](auto& itrs) {
    auto add_iterators = [&itrs](const sub_reader& /*segment*/,
                                 const doc_map_f& doc_map,
                                 const irs::column_reader& column) {
      auto it = column.iterator(ColumnHint::kConsolidation);

      if (IRS_LIKELY(it && irs::get<document>(*it))) {
        itrs.emplace_back(std::move(it), doc_map);
      } else {
        assert(false);
        IR_FRMT_ERROR(
          "Got an invalid iterator during consolidationg of the columnstore, "
          "skipping it");
      }
      return true;
    };

    itrs.clear();
    return column_itr.visit(add_iterators);
  };

  while (column_itr.next()) {
    // visit matched columns from merging segments and
    // write all survived values to the new segment
    if (!progress() || !columns.reset(add_iterators)) {
      return false;  // failed to visit all values
    }

    const std::string_view column_name = column_itr.value().name();

    const auto res = cs.insert(
      columns, column_info(column_name),
      [column_name](bstring&) { return column_name; },
      [](data_output& out, bytes_view payload) {
        if (!payload.empty()) {
          out.write_bytes(payload.data(), payload.size());
        }
      });

    if (!res.has_value()) {
      return false;  // failed to insert all values
    }
  }

  return true;
}

// Write field term data
template<typename Iterator>
bool write_fields(columnstore& cs, Iterator& feature_itr,
                  const flush_state& flush_state, const segment_meta& meta,
                  const feature_info_provider_t& column_info,
                  compound_field_iterator& field_itr,
                  const merge_writer::flush_progress_t& progress) {
  REGISTER_TIMER_DETAILED();
  assert(cs.valid());

  feature_map_t features;
  irs::type_info::type_id feature{};
  std::vector<bytes_view> hdrs;
  hdrs.reserve(field_itr.size());

  auto add_iterators = [&field_itr, &hdrs, &feature](auto& itrs) {
    auto add_iterators = [&itrs, &hdrs, &feature](const sub_reader& segment,
                                                  const doc_map_f& doc_map,
                                                  const field_meta& field) {
      const auto column = field.features.find(feature);

      if (column == field.features.end() ||
          !field_limits::valid(column->second)) {
        // field has no feature
        return true;
      }

      auto* reader = segment.column(column->second);

      // Tail columns can be removed if empty.
      if (reader) {
        auto it = reader->iterator(ColumnHint::kConsolidation);
        assert(it);

        if (IRS_LIKELY(it)) {
          hdrs.emplace_back(reader->payload());

          if (IRS_LIKELY(irs::get<document>(*it))) {
            itrs.emplace_back(std::move(it), doc_map);
          } else {
            assert(false);
            IR_FRMT_ERROR(
              "Failed to get document attribute from the iterator, skipping "
              "it");
          }
        }
      }

      return true;
    };

    hdrs.clear();
    itrs.clear();
    return field_itr.visit(add_iterators);
  };

  auto field_writer = meta.codec->get_field_writer(true);
  field_writer->prepare(flush_state);

  while (field_itr.next()) {
    features.clear();
    auto& field_meta = field_itr.meta();

    auto begin = field_meta.features.begin();
    auto end = field_meta.features.end();

    for (; begin != end; ++begin) {
      if (!progress()) {
        return false;
      }

      std::tie(feature, std::ignore) = *begin;

      if (!feature_itr.reset(add_iterators)) {
        return false;
      }

      const auto [info, factory] = column_info(feature);

      std::optional<field_id> res;
      auto feature_writer =
        factory ? (*factory)({hdrs.data(), hdrs.size()}) : nullptr;

      if (feature_writer) {
        auto value_writer = [writer = feature_writer.get()](
                              data_output& out, bytes_view payload) {
          writer->write(out, payload);
        };

        res = cs.insert(
          feature_itr, info,
          [feature_writer = std::move(feature_writer)](bstring& out) {
            feature_writer->finish(out);
            return std::string_view{};
          },
          std::move(value_writer));
      } else if (!factory) {  // Otherwise factory has failed to instantiate
                              // writer
        res = cs.insert(
          feature_itr, info, [](bstring&) { return std::string_view{}; },
          [](data_output& out, bytes_view payload) {
            if (!payload.empty()) {
              out.write_bytes(payload.data(), payload.size());
            }
          });
      }

      if (!res.has_value()) {
        return false;  // Failed to insert all values
      }

      features[feature] = res.value();
    }

    // write field terms
    auto terms = field_itr.iterator();

    field_writer->write(field_meta.name, field_meta.index_features, features,
                        *terms);
  }

  field_writer->end();
  field_writer.reset();

  return !field_itr.aborted();
}

// Computes doc_id_map and docs_count
doc_id_t compute_doc_ids(doc_id_map_t& doc_id_map, const sub_reader& reader,
                         doc_id_t next_id) noexcept {
  REGISTER_TIMER_DETAILED();
  // assume not a lot of space wasted if doc_limits::min() > 0
  try {
    doc_id_map.resize(reader.docs_count() + doc_limits::min(),
                      doc_limits::eof());
  } catch (...) {
    IR_FRMT_ERROR(
      "Failed to resize merge_writer::doc_id_map to accommodate "
      "element: " IR_UINT64_T_SPECIFIER,
      reader.docs_count() + doc_limits::min());

    return doc_limits::invalid();
  }

  for (auto docs_itr = reader.docs_iterator(); docs_itr->next(); ++next_id) {
    auto src_doc_id = docs_itr->value();

    assert(src_doc_id >= doc_limits::min());
    assert(src_doc_id < reader.docs_count() + doc_limits::min());
    doc_id_map[src_doc_id] = next_id;  // set to next valid doc_id
  }

  return next_id;
}

const merge_writer::flush_progress_t kProgressNoop = []() { return true; };

}  // namespace

namespace iresearch {

merge_writer::reader_ctx::reader_ctx(sub_reader::ptr reader) noexcept
  : reader{std::move(reader)},
    doc_map([](doc_id_t) noexcept { return doc_limits::eof(); }) {
  assert(this->reader);
}

merge_writer::merge_writer() noexcept
  : dir_(noop_directory::instance()),
    column_info_(nullptr),
    feature_info_(nullptr),
    comparator_(nullptr) {}

merge_writer::operator bool() const noexcept {
  return &dir_ != &noop_directory::instance();
}

bool merge_writer::flush(tracking_directory& dir,
                         index_meta::index_segment_t& segment,
                         const flush_progress_t& progress) {
  REGISTER_TIMER_DETAILED();
  assert(progress);
  assert(!comparator_);
  assert(column_info_ && *column_info_);
  assert(feature_info_ && *feature_info_);

  const size_t size = readers_.size();

  field_meta_map_t field_meta_map;
  compound_field_iterator fields_itr{size, progress};
  compound_column_iterator columns_itr{size};
  feature_set_t fields_features;
  IndexFeatures index_features{IndexFeatures::NONE};

  doc_iterator_container remapping_itrs{size};

  doc_id_t base_id = doc_limits::min();  // next valid doc_id

  // collect field meta and field term data
  for (auto& reader_ctx : readers_) {
    // ensured by merge_writer::add(...)
    assert(reader_ctx.reader);

    auto& reader = *reader_ctx.reader;
    const auto docs_count = reader.docs_count();

    if (reader.live_docs_count() == docs_count) {  // segment has no deletes
      const auto reader_base = base_id - doc_limits::min();
      base_id += docs_count;

      reader_ctx.doc_map = [reader_base](doc_id_t doc) noexcept {
        return reader_base + doc;
      };
    } else {  // segment has some deleted docs
      auto& doc_id_map = reader_ctx.doc_id_map;
      base_id = compute_doc_ids(doc_id_map, reader, base_id);

      reader_ctx.doc_map = [&doc_id_map](doc_id_t doc) noexcept {
        return doc >= doc_id_map.size() ? doc_limits::eof() : doc_id_map[doc];
      };
    }

    if (!doc_limits::valid(base_id)) {
      return false;  // failed to compute next doc_id
    }

    if (!compute_field_meta(field_meta_map, index_features, fields_features,
                            reader)) {
      return false;
    }

    fields_itr.add(reader, reader_ctx.doc_map);
    columns_itr.add(reader, reader_ctx.doc_map);
  }

  // total number of doc_ids
  segment.meta.docs_count = base_id - doc_limits::min();
  // all merged documents are live
  segment.meta.live_docs_count = segment.meta.docs_count;

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  // write merged segment data
  REGISTER_TIMER_DETAILED();
  columnstore cs(dir, segment.meta, progress);

  if (!cs.valid()) {
    return false;  // flush failure
  }

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  if (!write_columns(cs, remapping_itrs, *column_info_, columns_itr,
                     progress)) {
    return false;  // flush failure
  }

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  flush_state state;
  state.dir = &dir;
  state.doc_count = segment.meta.docs_count;
  state.features = &fields_features;
  state.index_features = index_features;
  state.name = segment.meta.name;

  // write field meta and field term data
  if (!write_fields(cs, remapping_itrs, state, segment.meta, *feature_info_,
                    fields_itr, progress)) {
    return false;  // flush failure
  }

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  segment.meta.column_store = cs.flush(state);

  return true;
}

bool merge_writer::flush_sorted(tracking_directory& dir,
                                index_meta::index_segment_t& segment,
                                const flush_progress_t& progress) {
  REGISTER_TIMER_DETAILED();
  assert(progress);
  assert(comparator_);
  assert(column_info_ && *column_info_);
  assert(feature_info_ && *feature_info_);

  const size_t size = readers_.size();

  field_meta_map_t field_meta_map;
  compound_column_iterator columns_itr{size};
  compound_field_iterator fields_itr{size, progress, comparator_};
  feature_set_t fields_features;
  IndexFeatures index_features{IndexFeatures::NONE};

  std::vector<SortingIteratorAdapter> itrs;
  itrs.reserve(size);

  auto emplace_iterator = [&itrs](const auto& segment) {
    if (!segment.sort()) {
      // sort column is not present, give up
      return false;
    }

    auto it =
      segment.mask(segment.sort()->iterator(irs::ColumnHint::kConsolidation));

    if (!it) {
      return false;
    }

    return itrs.emplace_back(std::move(it)).valid();
  };

  segment.meta.docs_count = 0;

  // Init doc map for each reader
  for (auto& reader_ctx : readers_) {
    // ensured by merge_writer::add(...)
    assert(reader_ctx.reader);

    auto& reader = *reader_ctx.reader;

    if (reader.docs_count() > doc_limits::eof() - doc_limits::min()) {
      // can't merge segment holding more than 'doc_limits::eof()-1' docs
      return false;
    }

    if (!emplace_iterator(reader)) {
      // sort column is not present, give up
      return false;
    }

    if (!compute_field_meta(field_meta_map, index_features, fields_features,
                            reader)) {
      return false;
    }

    fields_itr.add(reader, reader_ctx.doc_map);
    columns_itr.add(reader, reader_ctx.doc_map);

    // count total number of documents in consolidated segment
    if (!math::sum_check_overflow(segment.meta.docs_count,
                                  reader.live_docs_count(),
                                  segment.meta.docs_count)) {
      return false;
    }

    // prepare doc maps
    auto& doc_id_map = reader_ctx.doc_id_map;

    try {
      // assume not a lot of space wasted if doc_limits::min() > 0
      doc_id_map.resize(reader.docs_count() + doc_limits::min(),
                        doc_limits::eof());
    } catch (...) {
      IR_FRMT_ERROR(
        "Failed to resize merge_writer::doc_id_map to accommodate "
        "element: " IR_UINT64_T_SPECIFIER,
        reader.docs_count() + doc_limits::min());

      return false;
    }

    reader_ctx.doc_map = [&doc_id_map](doc_id_t doc) noexcept {
      return doc >= doc_id_map.size() ? doc_limits::eof() : doc_id_map[doc];
    };
  }

  if (segment.meta.docs_count >= doc_limits::eof()) {
    // can't merge segments holding more than 'doc_limits::eof()-1' docs
    return false;
  }

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  // Write new sorted column and fill doc maps for each reader
  SortingCompoundDocIterator columns_it{*comparator_, std::move(itrs)};

  auto writer = segment.meta.codec->get_columnstore_writer(true);
  writer->prepare(dir, segment.meta);

  // get column info for sorted column
  const auto info = (*column_info_)({});
  auto column = writer->push_column(info, {});

  for (doc_id_t next_id = doc_limits::min(); columns_it.next();) {
    const auto [index, it] = columns_it.value();
    assert(it);

    if (IRS_UNLIKELY(!it->valid())) {
      assert(false);
      IR_FRMT_ERROR(
        "Got an invalid iterator during consolidationg of sorted index, "
        "skipping it");
      continue;
    }

    auto& payload = it->payload->value;

    // fill doc id map
    readers_[index].doc_id_map[it->it->value()] = next_id;

    // write value into new column
    auto& stream = column.second(next_id);
    stream.write_bytes(payload.data(), payload.size());

    ++next_id;

    if (!progress()) {
      return false;  // progress callback requested termination
    }
  }

#ifdef IRESEARCH_DEBUG
  struct ne_eof {
    bool operator()(doc_id_t doc) const noexcept {
      return !doc_limits::eof(doc);
    }
  };

  // ensure doc ids for each segment are sorted
  for (auto& reader : readers_) {
    auto& doc_map = reader.doc_id_map;
    assert(doc_map.size() >= doc_limits::min());
    assert(std::is_sorted(
      boost::make_filter_iterator(ne_eof(), doc_map.begin(), doc_map.end()),
      boost::make_filter_iterator(ne_eof(), doc_map.end(), doc_map.end())));
    UNUSED(doc_map);
  }
#endif

  columnstore cs(std::move(writer), progress);
  compound_doc_iterator doc_it(progress);                // reuse iterator
  sorting_compound_doc_iterator sorting_doc_it(doc_it);  // reuse iterator

  if (!cs.valid()) {
    return false;  // flush failure
  }

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  if (!write_columns(cs, sorting_doc_it, *column_info_, columns_itr,
                     progress)) {
    return false;  // flush failure
  }

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  flush_state state;
  state.dir = &dir;
  state.doc_count = segment.meta.docs_count;
  state.index_features = index_features;
  state.features = &fields_features;
  state.name = segment.meta.name;

  // write field meta and field term data
  if (!write_fields(cs, sorting_doc_it, state, segment.meta, *feature_info_,
                    fields_itr, progress)) {
    return false;  // flush failure
  }

  if (!progress()) {
    return false;  // progress callback requested termination
  }

  segment.meta.column_store = cs.flush(state);  // flush columnstore
  segment.meta.sort = column.first;             // set sort column identifier
  // all merged documents are live
  segment.meta.live_docs_count = segment.meta.docs_count;

  return true;
}

bool merge_writer::flush(index_meta::index_segment_t& segment,
                         const flush_progress_t& progress /*= {}*/) {
  REGISTER_TIMER_DETAILED();
  assert(segment.meta.codec);  // must be set outside

  bool result = false;  // overall flush result

  Finally segment_invalidator = [&result, &segment]() noexcept {
    if (result) {
      // all good
      return;
    }

    // invalidate segment
    segment.filename.clear();
    auto& meta = segment.meta;
    meta.name.clear();
    meta.files.clear();
    meta.column_store = false;
    meta.docs_count = 0;
    meta.live_docs_count = 0;
    meta.size = 0;
    meta.version = 0;
  };

  const auto& progress_callback = progress ? progress : kProgressNoop;

  tracking_directory track_dir(dir_);  // track writer created files

  result = comparator_ ? flush_sorted(track_dir, segment, progress_callback)
                       : flush(track_dir, segment, progress_callback);

  track_dir.flush_tracked(segment.meta.files);

  return result;
}

}  // namespace iresearch
