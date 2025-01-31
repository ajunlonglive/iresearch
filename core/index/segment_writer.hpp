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

#ifndef IRESEARCH_SEGMENT_WRITER_H
#define IRESEARCH_SEGMENT_WRITER_H

#include <absl/container/node_hash_set.h>

#include "analysis/token_stream.hpp"
#include "column_info.hpp"
#include "field_data.hpp"
#include "formats/formats.hpp"
#include "sorted_column.hpp"
#include "utils/bitvector.hpp"
#include "utils/compression.hpp"
#include "utils/directory_utils.hpp"
#include "utils/noncopyable.hpp"
#include "utils/timer_utils.hpp"
#include "utils/type_limits.hpp"

namespace iresearch {

class comparer;
struct segment_meta;

// Defines how the inserting field should be processed
enum class Action {
  // Field should be indexed only
  // Field must satisfy 'Field' concept
  INDEX = 1,

  // Field should be stored only
  // Field must satisfy 'Attribute' concept
  STORE = 2,

  // Field should be stored in sorted order
  // Field must satisfy 'Attribute' concept
  STORE_SORTED = 4
};

ENABLE_BITMASK_ENUM(Action);

// Interface for an index writer over a directory
// an object that represents a single ongoing transaction
// non-thread safe
class segment_writer : util::noncopyable {
 private:
  // Disallow using public constructor
  struct ConstructToken {
    explicit ConstructToken() = default;
  };

 public:
  struct update_context {
    size_t generation;
    size_t update_id;
  };

  static std::unique_ptr<segment_writer> make(
    directory& dir, const column_info_provider_t& column_info,
    const feature_info_provider_t& feature_info, const comparer* comparator);

  // begin document-write transaction
  // Return doc_id_t as per type_limits<type_t::doc_id_t>
  doc_id_t begin(const update_context& ctx, size_t reserve_rollback_extra = 0);

  std::span<update_context> docs_context() noexcept { return docs_context_; }

  template<Action action, typename Field>
  bool insert(Field&& field) {
    if (IRS_LIKELY(valid_)) {
      if constexpr (Action::INDEX == action) {
        return index(std::forward<Field>(field));
      }

      if constexpr (Action::STORE == action) {
        return store(std::forward<Field>(field));
      }

      if constexpr (Action::STORE_SORTED == action) {
        return store_sorted(std::forward<Field>(field));
      }

      if constexpr ((Action::INDEX | Action::STORE) == action) {
        return index_and_store<false>(std::forward<Field>(field));
      }

      if constexpr ((Action::INDEX | Action::STORE_SORTED) == action) {
        return index_and_store<true>(std::forward<Field>(field));
      }

      assert(false);  // unsupported action
      valid_ = false;
    }

    return false;
  }

  // Commit document-write transaction
  void commit() {
    if (valid_) {
      finish();
    } else {
      rollback();
    }
  }

  // Return approximate amount of memory actively in-use by this instance
  size_t memory_active() const noexcept;

  // Return approximate amount of memory reserved by this instance
  size_t memory_reserved() const noexcept;

  // doc_id the document id as returned by begin(...)
  // Return success
  bool remove(doc_id_t doc_id);

  // Rollback document-write transaction,
  // implicitly noexcept since we reserve memory in 'begin'
  void rollback() {
    // mark as removed since not fully inserted

    // user should check return of begin() != eof()
    assert(docs_cached() + doc_limits::min() - 1 < doc_limits::eof());
    // -1 for 0-based offset
    remove(doc_id_t(docs_cached() + doc_limits::min() - 1));
    valid_ = false;
  }

  void flush(index_meta::index_segment_t& segment);

  const std::string& name() const noexcept { return seg_name_; }
  size_t docs_cached() const noexcept { return docs_context_.size(); }
  bool initialized() const noexcept { return initialized_; }
  bool valid() const noexcept { return valid_; }
  void reset() noexcept;
  void reset(const segment_meta& meta);

  void tick(uint64_t tick) noexcept { tick_ = tick; }
  uint64_t tick() const noexcept { return tick_; }

  segment_writer(ConstructToken, directory& dir,
                 const column_info_provider_t& column_info,
                 const feature_info_provider_t& feature_info,
                 const comparer* comparator) noexcept;

 private:
  struct stored_column : util::noncopyable {
    struct hash {
      using is_transparent = void;

      size_t operator()(const hashed_string_view& value) const noexcept {
        return value.hash();
      }

      size_t operator()(const stored_column& value) const noexcept {
        return value.name_hash;
      }
    };

    struct eq {
      using is_transparent = void;

      bool operator()(const stored_column& lhs,
                      const stored_column& rhs) const noexcept {
        return lhs.name == rhs.name;
      }

      bool operator()(const stored_column& lhs,
                      const hashed_string_view& rhs) const noexcept {
        return lhs.name == rhs;
      }

      bool operator()(const hashed_string_view& lhs,
                      const stored_column& rhs) const noexcept {
        return this->operator()(rhs, lhs);
      }
    };

    stored_column(const hashed_string_view& name,
                  columnstore_writer& columnstore,
                  const column_info_provider_t& column_info,
                  std::deque<cached_column>& cached_columns, bool cache);

    std::string name;
    size_t name_hash;
    columnstore_writer::values_writer_f writer;
    mutable field_id id{field_limits::invalid()};
  };

  // FIXME consider refactor this
  // we can't use flat_hash_set as stored_column stores 'this' in non-cached
  // case
  using stored_columns =
    absl::node_hash_set<stored_column, stored_column::hash, stored_column::eq>;

  struct sorted_column : util::noncopyable {
    explicit sorted_column(
      const column_info_provider_t& column_info,
      columnstore_writer::column_finalizer_f finalizer) noexcept
      : stream(column_info({})),  // compression for sorted column
        finalizer{std::move(finalizer)} {}

    field_id id{field_limits::invalid()};
    irs::sorted_column stream;
    columnstore_writer::column_finalizer_f finalizer;
  };

  bool index(const hashed_string_view& name, const doc_id_t doc,
             IndexFeatures index_features, const features_t& features,
             token_stream& tokens);

  template<typename Writer>
  bool store_sorted(const doc_id_t doc, Writer& writer) {
    assert(doc < doc_limits::eof());

    if (IRS_UNLIKELY(!fields_.comparator())) {
      // can't store sorted field without a comparator
      valid_ = false;
      return false;
    }

    auto& out = sorted_stream(doc);

    if (IRS_LIKELY(writer.write(out))) {
      return true;
    }

    out.reset();

    valid_ = false;
    return false;
  }

  template<typename Writer>
  bool store(const hashed_string_view& name, const doc_id_t doc,
             Writer& writer) {
    assert(doc < doc_limits::eof());

    auto& out = stream(name, doc);

    if (IRS_LIKELY(writer.write(out))) {
      return true;
    }

    out.reset();

    valid_ = false;
    return false;
  }

  template<typename Field>
  bool store(Field&& field) {
    REGISTER_TIMER_DETAILED();

    const auto field_name =
      make_hashed_ref(static_cast<std::string_view>(field.name()));

    // user should check return of begin() != eof()
    assert(docs_cached() + doc_limits::min() - 1 < doc_limits::eof());
    // -1 for 0-based offset
    const auto doc_id = doc_id_t(docs_cached() + doc_limits::min() - 1);

    return store(field_name, doc_id, field);
  }

  template<typename Field>
  bool store_sorted(Field&& field) {
    REGISTER_TIMER_DETAILED();

    // user should check return of begin() != eof()
    assert(docs_cached() + doc_limits::min() - 1 < doc_limits::eof());
    // -1 for 0-based offset
    const auto doc_id = doc_id_t(docs_cached() + doc_limits::min() - 1);

    return store_sorted(doc_id, field);
  }

  template<typename Field>
  bool index(Field&& field) {
    REGISTER_TIMER_DETAILED();

    const auto field_name =
      make_hashed_ref(static_cast<std::string_view>(field.name()));

    auto& tokens = static_cast<token_stream&>(field.get_tokens());
    const auto& features = static_cast<const features_t&>(field.features());
    const IndexFeatures index_features = field.index_features();

    // user should check return of begin() != eof()
    assert(docs_cached() + doc_limits::min() - 1 < doc_limits::eof());
    // -1 for 0-based offset
    const auto doc_id = doc_id_t(docs_cached() + doc_limits::min() - 1);

    return index(field_name, doc_id, index_features, features, tokens);
  }

  template<bool Sorted, typename Field>
  bool index_and_store(Field&& field) {
    REGISTER_TIMER_DETAILED();

    const auto field_name =
      make_hashed_ref(static_cast<std::string_view>(field.name()));

    auto& tokens = static_cast<token_stream&>(field.get_tokens());
    const auto& features = static_cast<const features_t&>(field.features());
    const IndexFeatures index_features = field.index_features();

    // user should check return of begin() != eof()
    assert(docs_cached() + doc_limits::min() - 1 < doc_limits::eof());
    // -1 for 0-based offset
    const auto doc_id = doc_id_t(docs_cached() + doc_limits::min() - 1);

    if (IRS_UNLIKELY(
          !index(field_name, doc_id, index_features, features, tokens))) {
      return false;  // indexing failed
    }

    if constexpr (Sorted) {
      return store_sorted(doc_id, field);
    }

    return store(field_name, doc_id, field);
  }

  // Returns stream for storing attributes in sorted order
  column_output& sorted_stream(const doc_id_t doc_id) {
    sort_.stream.prepare(doc_id);
    return sort_.stream;
  }

  // Returns stream for storing attributes
  column_output& stream(const hashed_string_view& name, const doc_id_t doc);

  // Finishes document
  void finish() {
    REGISTER_TIMER_DETAILED();
    for (const auto* field : doc_) {
      field->compute_features();
    }
  }

  // Flushes document mask to directory, returns number of masked documens
  size_t flush_doc_mask(const segment_meta& meta, const doc_map& docmap);
  // Flushes indexed fields to directory
  void flush_fields(const doc_map& docmap);

  std::deque<cached_column> cached_columns_;  // pointers remain valid
  sorted_column sort_;
  std::vector<update_context> docs_context_;
  // invalid/removed doc_ids (e.g. partially indexed due to indexing failure)
  bitvector docs_mask_;
  fields_data fields_;
  stored_columns columns_;
  std::vector<const stored_column*> sorted_columns_;
  std::vector<const field_data*> doc_;  // document fields
  std::string seg_name_;
  field_writer::ptr field_writer_;
  const column_info_provider_t* column_info_;
  columnstore_writer::ptr col_writer_;
  tracking_directory dir_;
  uint64_t tick_{0};
  bool initialized_;
  bool valid_{true};  // current state
};

}  // namespace iresearch

#endif  // IRESEARCH_SEGMENT_WRITER_H
