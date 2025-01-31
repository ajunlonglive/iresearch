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

#include "index_writer.hpp"

#include <absl/container/flat_hash_map.h>

#include <sstream>

#include "formats/format_utils.hpp"
#include "index/comparer.hpp"
#include "index/file_names.hpp"
#include "index/merge_writer.hpp"
#include "search/exclusion.hpp"
#include "shared.hpp"
#include "utils/bitvector.hpp"
#include "utils/compression.hpp"
#include "utils/directory_utils.hpp"
#include "utils/index_utils.hpp"
#include "utils/string_utils.hpp"
#include "utils/timer_utils.hpp"
#include "utils/type_limits.hpp"

namespace iresearch {
namespace {

constexpr size_t kNonUpdateRecord = std::numeric_limits<size_t>::max();

// do-nothing progress reporter, used as fallback if no other progress
// reporter is used
const index_writer::progress_report_callback kNoProgress =
  [](std::string_view /*phase*/, size_t /*current*/, size_t /*total*/) {
    // intentionally do nothing
  };

const column_info_provider_t kDefaultColumnInfo = [](std::string_view) {
  // no compression, no encryption
  return column_info{irs::type<compression::none>::get(), {}, false};
};

const feature_info_provider_t kDefaultFeatureInfo =
  [](irs::type_info::type_id) {
    // no compression, no encryption
    return std::make_pair(
      column_info{irs::type<compression::none>::get(), {}, false},
      feature_writer_factory_t{});
  };

struct flush_segment_context {
  // starting doc_id to consider in 'segment.meta' (inclusive)
  const size_t doc_id_begin_;
  // ending doc_id to consider in 'segment.meta' (exclusive)
  const size_t doc_id_end_;
  // doc_ids masked in segment_meta
  document_mask docs_mask_;
  // copy so that it can be moved into 'index_writer::pending_state_'
  index_meta::index_segment_t segment_;
  // modification contexts referenced by 'update_contexts_'
  std::span<const index_writer::modification_context> modification_contexts_;
  // update contexts for documents in segment_meta
  std::span<const segment_writer::update_context> update_contexts_;

  flush_segment_context(
    const index_meta::index_segment_t& segment, size_t doc_id_begin,
    size_t doc_id_end,
    std::span<const segment_writer::update_context> update_contexts,
    std::span<const index_writer::modification_context> modification_contexts)
    : doc_id_begin_(doc_id_begin),
      doc_id_end_(doc_id_end),
      segment_(segment),
      modification_contexts_{modification_contexts},
      update_contexts_{update_contexts} {
    assert(doc_id_begin_ <= doc_id_end_);
    assert(doc_id_end_ - doc_limits::min() <= segment_.meta.docs_count);
    assert(update_contexts.size() == segment_.meta.docs_count);
  }
};

std::vector<index_file_refs::ref_t> extract_refs(
  const ref_tracking_directory& dir) {
  std::vector<index_file_refs::ref_t> refs;
  // FIXME reserve

  auto visitor = [&refs](const index_file_refs::ref_t& ref) {
    refs.emplace_back(ref);
    return true;
  };
  dir.visit_refs(visitor);

  return refs;
}

// Apply any document removals based on filters in the segment.
// modifications where to get document update_contexts from
// docs_mask where to apply document removals to
// readers readers by segment name
// meta key used to get reader for the segment to evaluate
// min_modification_generation smallest consider modification generation
// Return if any new records were added (modification_queries_ modified).
bool add_document_mask_modified_records(
  std::span<index_writer::modification_context> modifications,
  document_mask& docs_mask, readers_cache& readers, segment_meta& meta,
  size_t min_modification_generation = 0) {
  if (modifications.empty()) {
    return false;  // nothing new to flush
  }

  auto reader = readers.emplace(meta);

  if (!reader) {
    throw index_error(string_utils::to_string(
      "while adding document mask modified records to document_mask of segment "
      "'%s', error: failed to open segment",
      meta.name.c_str()));
  }

  bool modified = false;

  for (auto& modification : modifications) {
    if (!modification.filter) {
      continue;  // skip invalid or uncommitted modification queries
    }

    auto prepared = modification.filter->prepare(reader);

    if (!prepared) {
      continue;  // skip invalid prepared filters
    }

    auto itr = prepared->execute(reader);

    if (!itr) {
      continue;  // skip invalid iterators
    }

    while (itr->next()) {
      const auto doc_id = itr->value();

      // if the indexed doc_id was insert()ed after the request for modification
      // or the indexed doc_id was already masked then it should be skipped
      if (modification.generation < min_modification_generation ||
          !docs_mask.insert(doc_id).second) {
        continue;  // the current modification query does not match any records
      }

      assert(meta.live_docs_count);
      --meta.live_docs_count;  // decrement count of live docs
      modification.seen = true;
      modified = true;
    }
  }

  return modified;
}

// Apply any document removals based on filters in the segment.
// modifications where to get document update_contexts from
// segment where to apply document removals to
// min_doc_id staring doc_id that should be considered
// readers readers by segment name
// Return if any new records were added (modification_queries_ modified).
bool add_document_mask_modified_records(
  std::span<index_writer::modification_context> modifications,
  flush_segment_context& ctx, readers_cache& readers) {
  if (modifications.empty()) {
    return false;  // nothing new to flush
  }

  auto& segment = ctx.segment_;
  auto reader = readers.emplace(segment.meta);

  if (!reader) {
    throw index_error(string_utils::to_string(
      "while adding document mask modified records to flush_segment_context of "
      "segment '%s', error: failed to open segment",
      segment.meta.name.c_str()));
  }

  assert(doc_limits::valid(ctx.doc_id_begin_));
  assert(ctx.doc_id_begin_ <= ctx.doc_id_end_);
  assert(ctx.doc_id_end_ <= ctx.update_contexts_.size() + doc_limits::min());
  bool modified = false;

  for (auto& modification : modifications) {
    if (!modification.filter) {
      continue;  // skip invalid or uncommitted modification queries
    }

    auto prepared = modification.filter->prepare(reader);

    if (!prepared) {
      continue;  // skip invalid prepared filters
    }

    auto itr = prepared->execute(reader);

    if (!itr) {
      continue;  // skip invalid iterators
    }

    while (itr->next()) {
      const auto doc_id = itr->value();

      if (doc_id < ctx.doc_id_begin_ || doc_id >= ctx.doc_id_end_) {
        continue;  // doc_id is not part of the current flush_context
      }

      // valid because of asserts above
      auto& doc_ctx = ctx.update_contexts_[doc_id - doc_limits::min()];

      // if the indexed doc_id was insert()ed after the request for modification
      // or the indexed doc_id was already masked then it should be skipped
      if (modification.generation < doc_ctx.generation ||
          !ctx.docs_mask_.insert(doc_id).second) {
        continue;  // the current modification query does not match any records
      }

      // if an update modification and update-value record whose query was not
      // seen (i.e. replacement value whose filter did not match any documents)
      // for every update request a replacement 'update-value' is optimistically
      // inserted
      if (modification.update && doc_ctx.update_id != kNonUpdateRecord &&
          !ctx.modification_contexts_[doc_ctx.update_id].seen) {
        continue;  // the current modification matched a replacement document
                   // which in turn did not match any records
      }

      assert(segment.meta.live_docs_count);
      --segment.meta.live_docs_count;  // decrement count of live docs
      modification.seen = true;
      modified = true;
    }
  }

  return modified;
}

// Mask documents created by updates which did not have any matches.
// Return if any new records were added (modification_contexts_ modified).
bool add_document_mask_unused_updates(flush_segment_context& ctx) {
  if (ctx.modification_contexts_.empty()) {
    return false;  // nothing new to add
  }
  assert(doc_limits::valid(ctx.doc_id_begin_));
  assert(ctx.doc_id_begin_ <= ctx.doc_id_end_);
  assert(ctx.doc_id_end_ <= ctx.update_contexts_.size() + doc_limits::min());
  bool modified = false;

  for (auto doc_id = ctx.doc_id_begin_; doc_id < ctx.doc_id_end_; ++doc_id) {
    // valid because of asserts above
    auto& doc_ctx = ctx.update_contexts_[doc_id - doc_limits::min()];

    if (doc_ctx.update_id == kNonUpdateRecord) {
      continue;  // not an update operation
    }

    assert(ctx.modification_contexts_.size() > doc_ctx.update_id);

    // if it's an update record placeholder who's query already match some
    // record
    if (ctx.modification_contexts_[doc_ctx.update_id].seen ||
        !ctx.docs_mask_.insert(doc_id).second) {
      continue;  // the current placeholder record is in-use and valid
    }

    assert(ctx.segment_.meta.live_docs_count);
    --ctx.segment_.meta.live_docs_count;  // decrement count of live docs
    modified = true;
  }

  return modified;
}

// append file refs for files from the specified segments description
template<typename T, typename M>
void append_segments_refs(T& buf, directory& dir, const M& meta) {
  auto visitor = [&buf](const index_file_refs::ref_t& ref) -> bool {
    buf.emplace_back(ref);
    return true;
  };

  // track all files referenced in index_meta
  directory_utils::reference(dir, meta, visitor, true);
}

std::string_view write_document_mask(directory& dir, segment_meta& meta,
                                     const document_mask& docs_mask,
                                     bool increment_version = true) {
  assert(docs_mask.size() <= std::numeric_limits<uint32_t>::max());

  auto mask_writer = meta.codec->get_document_mask_writer();

  if (increment_version) {
    meta.files.erase(mask_writer->filename(meta));  // current filename
    ++meta.version;  // segment modified due to new document_mask

    // a second time +1 to avoid overlap with version increment due to commit of
    // uncommited segment tail which must mask committed segment head
    // NOTE0: +1 extra is enough since a segment can reside in at most 2
    //        flush_contexts, there fore no more than 1 tail
    // NOTE1: flush_all() Stage3 increments version by _only_ 1 to avoid overlap
    //        with here, i.e. segment tail version will always be odd due to the
    //        aforementioned and because there is at most 1 tail
    ++meta.version;
  }

  const auto [file, _] =
    meta.files.emplace(mask_writer->filename(meta));  // new/expected filename

  mask_writer->write(dir, meta, docs_mask);

  // reset no longer valid size, to be recomputed on
  // index_utils::write_index_segment(...)
  meta.size = 0;

  return *file;
}

// mapping: name -> { new segment, old segment }
using candidates_mapping_t = absl::flat_hash_map<
  std::string_view,
  std::pair<const segment_meta*,                       // new segment
            std::pair<const segment_meta*, size_t>>>;  // old segment + index
                                                       // within merge_writer

// candidates_mapping output mapping
// candidates candidates for mapping
// segments map against a specified segments
// Returns first - has removals, second - number of mapped candidates.
std::pair<bool, size_t> map_candidates(
  candidates_mapping_t& candidates_mapping,
  const index_writer::consolidation_t& candidates,
  const index_meta::index_segments_t& segments) {
  size_t i = 0;
  for (auto* candidate : candidates) {
    candidates_mapping.emplace(
      std::piecewise_construct, std::forward_as_tuple(candidate->name),
      std::forward_as_tuple(nullptr, std::make_pair(candidate, i++)));
  }

  size_t found = 0;
  bool has_removals = false;
  const auto candidate_not_found = candidates_mapping.end();

  for (const auto& segment : segments) {
    const auto& meta = segment.meta;
    const auto it = candidates_mapping.find(meta.name);

    if (candidate_not_found == it) {
      // not a candidate
      continue;
    }

    auto& mapping = it->second;
    auto* new_segment = mapping.first;

    if (new_segment && new_segment->version >= meta.version) {
      // mapping already has a newer segment version
      continue;
    }

    ++found;

    assert(mapping.second.first);
    mapping.first = &meta;

    has_removals |= (meta.version != it->second.second.first->version);
  }

  return std::make_pair(has_removals, found);
}

bool map_removals(const candidates_mapping_t& candidates_mapping,
                  const merge_writer& merger, readers_cache& readers,
                  document_mask& docs_mask) {
  assert(merger);

  for (auto& mapping : candidates_mapping) {
    const auto& segment_mapping = mapping.second;

    if (segment_mapping.first->version !=
        segment_mapping.second.first->version) {
      auto& merge_ctx = merger[segment_mapping.second.second];
      auto reader = readers.emplace(*segment_mapping.first);
      auto merged_itr = merge_ctx.reader->docs_iterator();
      auto current_itr = reader->docs_iterator();

      // this only masks documents of a single segment
      // this works due to the current architectural approach of segments,
      // either removals are new and will be applied during flush_all()
      // or removals are in the docs_mask and swill be applied by the reader
      // passed to the merge_writer

      // no more docs in merged reader
      if (!merged_itr->next()) {
        if (current_itr->next()) {
          IR_FRMT_WARN(
            "Failed to map removals for consolidated segment '%s' version "
            "'" IR_UINT64_T_SPECIFIER
            "' from current segment '%s' version '" IR_UINT64_T_SPECIFIER
            "', current segment has doc_id '" IR_UINT32_T_SPECIFIER
            "' not present in the consolidated segment",
            segment_mapping.second.first->name.c_str(),
            segment_mapping.second.first->version,
            segment_mapping.first->name.c_str(), segment_mapping.first->version,
            current_itr->value());

          return false;  // current reader has unmerged docs
        }

        continue;  // continue wih next mapping
      }

      // mask all remaining doc_ids
      if (!current_itr->next()) {
        do {
          assert(doc_limits::valid(merge_ctx.doc_map(
            merged_itr->value())));  // doc_id must have a valid mapping
          docs_mask.insert(merge_ctx.doc_map(merged_itr->value()));
        } while (merged_itr->next());

        continue;  // continue wih next mapping
      }

      // validate that all docs in the current reader were merged, and add any
      // removed docs to the meged mask
      for (;;) {
        while (merged_itr->value() < current_itr->value()) {
          // doc_id must have a valid mapping
          assert(doc_limits::valid(merge_ctx.doc_map(merged_itr->value())));
          docs_mask.insert(merge_ctx.doc_map(merged_itr->value()));

          if (!merged_itr->next()) {
            IR_FRMT_WARN(
              "Failed to map removals for consolidated segment '%s' version "
              "'" IR_UINT64_T_SPECIFIER
              "' from current segment '%s' version '" IR_UINT64_T_SPECIFIER
              "', current segment has doc_id '" IR_UINT32_T_SPECIFIER
              "' not present in the consolidated segment",
              segment_mapping.second.first->name.c_str(),
              segment_mapping.second.first->version,
              segment_mapping.first->name.c_str(),
              segment_mapping.first->version, current_itr->value());

            return false;  // current reader has unmerged docs
          }
        }

        if (merged_itr->value() > current_itr->value()) {
          IR_FRMT_WARN(
            "Failed to map removals for consolidated segment '%s' version "
            "'" IR_UINT64_T_SPECIFIER
            "' from current segment '%s' version '" IR_UINT64_T_SPECIFIER
            "', current segment has doc_id '" IR_UINT32_T_SPECIFIER
            "' not present in the consolidated segment",
            segment_mapping.second.first->name.c_str(),
            segment_mapping.second.first->version,
            segment_mapping.first->name.c_str(), segment_mapping.first->version,
            current_itr->value());

          return false;  // current reader has unmerged docs
        }

        // no more docs in merged reader
        if (!merged_itr->next()) {
          if (current_itr->next()) {
            IR_FRMT_WARN(
              "Failed to map removals for consolidated segment '%s' version "
              "'" IR_UINT64_T_SPECIFIER
              "' from current segment '%s' version '" IR_UINT64_T_SPECIFIER
              "', current segment has doc_id '" IR_UINT32_T_SPECIFIER
              "' not present in the consolidated segment",
              segment_mapping.second.first->name.c_str(),
              segment_mapping.second.first->version,
              segment_mapping.first->name.c_str(),
              segment_mapping.first->version, current_itr->value());

            return false;  // current reader has unmerged docs
          }

          break;  // continue wih next mapping
        }

        // mask all remaining doc_ids
        if (!current_itr->next()) {
          do {
            // doc_id must have a valid mapping
            assert(doc_limits::valid(merge_ctx.doc_map(merged_itr->value())));
            docs_mask.insert(merge_ctx.doc_map(merged_itr->value()));
          } while (merged_itr->next());

          break;  // continue wih next mapping
        }
      }
    }
  }

  return true;
}

std::string to_string(const index_writer::consolidation_t& consolidation) {
  std::stringstream ss;
  size_t total_size = 0;
  size_t total_docs_count = 0;
  size_t total_live_docs_count = 0;

  for (const auto* meta : consolidation) {
    ss << "Name='" << meta->name << "', docs_count=" << meta->docs_count
       << ", live_docs_count=" << meta->live_docs_count
       << ", size=" << meta->size << std::endl;

    total_docs_count += meta->docs_count;
    total_live_docs_count += meta->live_docs_count;
    total_size += meta->size;
  }

  ss << "Total: segments=" << consolidation.size()
     << ", docs_count=" << total_docs_count
     << ", live_docs_count=" << total_live_docs_count << " size=" << total_size
     << "";

  return ss.str();
}

}  // namespace

using namespace std::chrono_literals;

readers_cache::key_t::key_t(const segment_meta& meta)
  : name(meta.name), version(meta.version) {}

segment_reader readers_cache::emplace(const segment_meta& meta) {
  REGISTER_TIMER_DETAILED();

  segment_reader cached_reader;

  // FIXME(gnusi) consider moving open/reopen out of the scope of the lock
  // cppcheck-suppress unreadVariable
  std::lock_guard lock{lock_};
  auto& reader = cache_[meta];

  cached_reader = std::move(reader);  // clear existing reader

  // update cache, in case of failure reader stays empty
  // intentionally never warmup readers for writers
  reader = cached_reader
             ? cached_reader.reopen(meta)
             : segment_reader::open(dir_, meta, index_reader_options{});

  return reader;
}

void readers_cache::clear() noexcept {
  // cppcheck-suppress unreadVariable
  std::lock_guard lock{lock_};
  cache_.clear();
}

size_t readers_cache::purge(
  const absl::flat_hash_set<key_t, key_hash_t>& segments) noexcept {
  if (segments.empty()) {
    return 0;
  }

  size_t erased = 0;

  // cppcheck-suppress unreadVariable
  std::lock_guard lock{lock_};

  for (auto it = cache_.begin(); it != cache_.end();) {
    if (segments.contains(it->first)) {
      const auto erase_me = it++;
      cache_.erase(erase_me);
      ++erased;
    } else {
      ++it;
    }
  }

  return erased;
}

index_writer::active_segment_context::active_segment_context(
  std::shared_ptr<segment_context> ctx, std::atomic<size_t>& segments_active,
  flush_context* flush_ctx, size_t pending_segment_context_offset) noexcept
  : ctx_{std::move(ctx)},
    flush_ctx_{flush_ctx},
    pending_segment_context_offset_{pending_segment_context_offset},
    segments_active_{&segments_active} {
#ifdef IRESEARCH_DEBUG
  if (flush_ctx) {
    // ensure there are no active struct update operations
    // (only needed for assert)
    // cppcheck-suppress unreadVariable
    std::lock_guard lock{flush_ctx->mutex_};

    // assert that flush_ctx and ctx are compatible
    assert(flush_ctx->pending_segment_contexts_[pending_segment_context_offset_]
             .segment_ == ctx_);
  }
#endif

  if (ctx_) {
    // track here since garanteed to have 1 ref per active
    // segment
    ++*segments_active_;
  }
}

index_writer::active_segment_context::~active_segment_context() {
  if (ctx_) {
    // track here since garanteed to have 1 ref per active
    // segment
    --*segments_active_;
  }

  if (flush_ctx_) {
    ctx_.reset();

    try {
      std::lock_guard lock{flush_ctx_->mutex_};
      flush_ctx_->pending_segment_context_cond_.notify_all();
    } catch (...) {
      // lock may throw
    }
  }  // FIXME TODO remove once col_writer tail is fixed to flush() multiple
     // times without overwrite (since then the tail will be in a different
     // context)
}

index_writer::active_segment_context&
index_writer::active_segment_context::operator=(
  active_segment_context&& other) noexcept {
  if (this != &other) {
    if (ctx_) {
      // track here since garanteed to have 1 ref per active segment
      --*segments_active_;
    }

    ctx_ = std::move(other.ctx_);
    flush_ctx_ = other.flush_ctx_;
    pending_segment_context_offset_ = other.pending_segment_context_offset_;
    segments_active_ = other.segments_active_;
  }

  return *this;
}

index_writer::document::document(flush_context& ctx,
                                 std::shared_ptr<segment_context> segment,
                                 const segment_writer::update_context& update)
  : segment_{std::move(segment)},
    writer_{*segment_->writer_},
    ctx_{ctx},
    update_id_{update.update_id} {
  assert(segment_);
  assert(segment_->writer_);
  const auto uncomitted_doc_id_begin =
    segment_->uncomitted_doc_id_begin_ >
        segment_->flushed_update_contexts_.size()
      // uncomitted start in 'writer_'
      ? (segment_->uncomitted_doc_id_begin_ -
         segment_->flushed_update_contexts_.size())
      // uncommited start in 'flushed_'
      : doc_limits::min();
  assert(uncomitted_doc_id_begin <= writer_.docs_cached() + doc_limits::min());
  // ensure reset() will be noexcept
  ++segment_->active_count_;
  const auto rollback_extra =
    writer_.docs_cached() + doc_limits::min() - uncomitted_doc_id_begin;
  writer_.begin(update, rollback_extra);  // ensure reset() will be noexcept
  segment_->buffered_docs_.store(writer_.docs_cached());
}

index_writer::document::~document() noexcept {
  if (!segment_) {
    return;  // another instance will call commit()
  }

  assert(segment_->writer_);
  assert(&writer_ == segment_->writer_.get());

  try {
    writer_.commit();
  } catch (...) {
    writer_.rollback();
  }

  if (!*this && update_id_ != kNonUpdateRecord) {
    // mark invalid
    segment_->modification_queries_[update_id_].filter = nullptr;
  }

  // optimization to notify any ongoing flush_all() operations so they wake up
  // earlier
  if (!--segment_->active_count_) {
    // lock due to context modification and notification, note:
    // std::mutex::try_lock() does not throw exceptions as per documentation
    // @see https://en.cppreference.com/w/cpp/named_req/Mutex
    std::unique_lock lock{ctx_.mutex_, std::try_to_lock};

    if (lock.owns_lock()) {
      // ignore if lock failed because it imples that
      // flush_all() is not waiting for a notification
      ctx_.pending_segment_context_cond_.notify_all();
    }
  }
}

void index_writer::documents_context::AddToFlush() {
  if (const auto& ctx = segment_.ctx(); !ctx) {
    return;  // nothing to do
  }

  writer_.get_flush_context()->AddToPending(segment_);
}

index_writer::documents_context::~documents_context() noexcept {
  auto& ctx = segment_.ctx();

  // failure may indicate a dangling 'document' instance
  assert(ctx.use_count() >= 0 &&
         static_cast<uint64_t>(ctx.use_count()) == segment_use_count_);

  if (!ctx) {
    return;  // nothing to do
  }

  if (auto& writer = *ctx->writer_; writer.tick() < last_operation_tick_) {
    writer.tick(last_operation_tick_);
  }

  try {
    // FIXME move emplace into active_segment_context destructor commit segment
    writer_.get_flush_context()->emplace(std::move(segment_),
                                         first_operation_tick_);
  } catch (...) {
    reset();  // abort segment
  }
}

void index_writer::documents_context::reset() noexcept {
  last_operation_tick_ = 0;  // reset tick

  auto& ctx = segment_.ctx();

  if (!ctx) {
    return;  // nothing to reset
  }

  // rollback modification queries
  std::for_each(std::begin(ctx->modification_queries_) +
                  ctx->uncomitted_modification_queries_,
                std::end(ctx->modification_queries_),
                [](modification_context& ctx) noexcept {
                  // mark invalid
                  ctx.filter = nullptr;
                });

  auto& flushed_update_contexts = ctx->flushed_update_contexts_;

  // find and mask/truncate uncomitted tail
  for (size_t i = 0, count = ctx->flushed_.size(), flushed_docs_count = 0;
       i < count; ++i) {
    auto& segment = ctx->flushed_[i];
    auto flushed_docs_start = flushed_docs_count;

    // sum of all previous segment_meta::docs_count
    // including this meta
    flushed_docs_count += segment.meta.docs_count;

    if (flushed_docs_count <=
        ctx->uncomitted_doc_id_begin_ - doc_limits::min()) {
      continue;  // all documents in this this index_meta have been commited
    }

    auto docs_mask_tail_doc_id =
      ctx->uncomitted_doc_id_begin_ - flushed_docs_start;

    assert(docs_mask_tail_doc_id <= segment.meta.live_docs_count);
    assert(docs_mask_tail_doc_id <= std::numeric_limits<doc_id_t>::max());
    segment.docs_mask_tail_doc_id = doc_id_t(docs_mask_tail_doc_id);

    if (docs_mask_tail_doc_id - doc_limits::min() >= segment.meta.docs_count) {
      ctx->flushed_.resize(i);  // truncate including current empty meta
    } else {
      ctx->flushed_.resize(i + 1);  // truncate starting from subsequent meta
    }

    assert(flushed_update_contexts.size() >= flushed_docs_count);
    // truncate 'flushed_update_contexts_'
    flushed_update_contexts.resize(flushed_docs_count);
    // reset to start of 'writer_'
    ctx->uncomitted_doc_id_begin_ =
      flushed_update_contexts.size() + doc_limits::min();

    break;
  }

  if (!ctx->writer_) {
    assert(ctx->uncomitted_doc_id_begin_ - doc_limits::min() ==
           flushed_update_contexts.size());
    ctx->buffered_docs_.store(flushed_update_contexts.size());

    return;  // nothing to reset
  }

  auto& writer = *(ctx->writer_);
  auto writer_docs = writer.initialized() ? writer.docs_cached() : 0;

  assert(std::numeric_limits<doc_id_t>::max() >= writer.docs_cached());
  // update_contexts located inside th writer
  assert(ctx->uncomitted_doc_id_begin_ - doc_limits::min() >=
         flushed_update_contexts.size());
  assert(ctx->uncomitted_doc_id_begin_ - doc_limits::min() <=
         flushed_update_contexts.size() + writer_docs);
  ctx->buffered_docs_.store(flushed_update_contexts.size() + writer_docs);

  // rollback document insertions
  // cannot segment_writer::reset(...) since documents_context::reset() noexcept
  for (auto doc_id =
              ctx->uncomitted_doc_id_begin_ - flushed_update_contexts.size(),
            doc_id_end = writer_docs + doc_limits::min();
       doc_id < doc_id_end; ++doc_id) {
    assert(doc_id <= std::numeric_limits<doc_id_t>::max());
    writer.remove(doc_id_t(doc_id));
  }
}

index_writer::flush_context_ptr index_writer::documents_context::update_segment(
  bool disable_flush) {
  auto ctx = writer_.get_flush_context();

  // refresh segment if required (guarded by flush_context::flush_mutex_)

  while (!segment_.ctx()) {  // no segment (lazy initialized)
    segment_ = writer_.get_segment_context(*ctx);
    segment_use_count_ = segment_.ctx().use_count();

    // must unlock/relock flush_context before retrying to get a new segment so
    // as to avoid a deadlock due to a read-write-read situation for
    // flush_context::flush_mutex_ with threads trying to lock
    // flush_context::flush_mutex_ to return their segment_context
    if (!segment_.ctx()) {
      ctx.reset();  // reset before reaquiring
      ctx = writer_.get_flush_context();
    }
  }

  assert(segment_.ctx());
  assert(segment_.ctx()->writer_);
  auto& segment = *(segment_.ctx());
  auto& writer = *segment.writer_;

  if (writer.initialized() && !disable_flush) {
    const auto& segment_limits = writer_.segment_limits_;
    const auto segment_docs_max = segment_limits.segment_docs_max.load();
    const auto segment_memory_max = segment_limits.segment_memory_max.load();

    // if not reached the limit of the current segment then use it
    if ((!segment_docs_max ||
         segment_docs_max > writer.docs_cached())  // too many docs
        && (!segment_memory_max ||
            segment_memory_max > writer.memory_active())  // too much memory
        && !doc_limits::eof(writer.docs_cached())) {      // segment full
      return ctx;
    }

    // force a flush of a full segment
    IR_FRMT_TRACE("Flushing segment '%s', docs=" IR_SIZE_T_SPECIFIER
                  ", memory=" IR_SIZE_T_SPECIFIER
                  ", docs limit=" IR_SIZE_T_SPECIFIER
                  ", memory limit=" IR_SIZE_T_SPECIFIER "",
                  writer.name().c_str(), writer.docs_cached(),
                  writer.memory_active(), segment_docs_max, segment_memory_max);

    try {
      std::unique_lock segment_flush_lock{segment.flush_mutex_};
      segment.flush();
    } catch (...) {
      IR_FRMT_ERROR(
        "while flushing segment '%s', error: failed to flush segment",
        segment.writer_meta_.meta.name.c_str());

      segment.reset(true);

      throw;
    }
  }

  segment.prepare();
  assert(segment.writer_->initialized());

  return ctx;
}

void index_writer::flush_context::emplace(active_segment_context&& segment,
                                          uint64_t generation_base) {
  if (!segment.ctx_) {
    return;  // nothing to do
  }

  auto& flush_ctx = segment.flush_ctx_;

  // failure may indicate a dangling 'document' instance
  assert(
    // +1 for 'active_segment_context::ctx_'
    (!flush_ctx && segment.ctx_.use_count() == 1) ||
    // +1 for 'active_segment_context::ctx_' (flush_context switching made a
    // full-circle)
    (this == flush_ctx && segment.ctx_->dirty_ &&
     segment.ctx_.use_count() == 1) ||
    // +1 for 'active_segment_context::ctx_', +1 for
    // 'pending_segment_context::segment_'
    (this == flush_ctx && !segment.ctx_->dirty_ &&
     segment.ctx_.use_count() == 2) ||
    // +1 for 'active_segment_context::ctx_', +1 for
    // 'pending_segment_context::segment_'
    (this != flush_ctx && flush_ctx && segment.ctx_.use_count() == 2) ||
    // +1 for 'active_segment_context::ctx_', +0 for
    // 'pending_segment_context::segment_' that was already cleared
    (this != flush_ctx && flush_ctx && segment.ctx_.use_count() == 1));

  freelist_t::node_type* freelist_node = nullptr;
  size_t modification_count{};
  auto& ctx = *(segment.ctx_);

  // prevent concurrent flush related modifications,
  // i.e. if segment is also owned by another flush_context
  std::unique_lock flush_lock{ctx.flush_mutex_, std::defer_lock};

  {
    // pending_segment_contexts_ may be asynchronously read
    std::lock_guard lock{mutex_};

    // update pending_segment_context
    // this segment_context has not yet been seen by this flush_context
    // or was marked dirty imples flush_context switching making a full-circle
    if (this != flush_ctx || ctx.dirty_) {
      freelist_node = &pending_segment_contexts_.emplace_back(
        segment.ctx_, pending_segment_contexts_.size());

      // mark segment as non-reusable if it was peviously registered with a
      // different flush_context NOTE: 'ctx.dirty_' implies flush_context
      // switching making a full-circle
      //       and this emplace(...) call being the first and only call for this
      //       segment (not given out again via free-list) so no 'dirty_' check
      if (flush_ctx && this != flush_ctx) {
        ctx.dirty_ = true;
        // 'segment.flush_ctx_' may be asynchronously flushed
        flush_lock.lock();
        // thread-safe because pending_segment_contexts_ is a deque
        assert(
          flush_ctx
            ->pending_segment_contexts_[segment.pending_segment_context_offset_]
            .segment_ == segment.ctx_);
        // ^^^ FIXME TODO remove last line
        /* FIXME TODO uncomment once col_writer tail is writen correctly (need
        to track tail in new segment
        // if this segment is still referenced by the previous flush_context
        then
        // store 'pending_segment_contexts_' and
        'uncomitted_modification_queries_'
        // in the previous flush_context because they will be modified lower
        down if (segment.ctx_.use_count() != 2) {
          assert(segment.flush_ctx_->pending_segment_contexts_.size() >
        segment.pending_segment_context_offset_);
          assert(segment.flush_ctx_->pending_segment_contexts_[segment.pending_segment_context_offset_].segment_
        == segment.ctx_); // thread-safe because pending_segment_contexts_ is a
        deque
          assert(segment.flush_ctx_->pending_segment_contexts_[segment.pending_segment_context_offset_].segment_.use_count()
        == 3); // +1 for the reference in 'pending_segment_contexts_', +1 for
        the reference in other flush_context 'pending_segment_contexts_', +1 for
        the reference in 'active_segment_context'
          segment.flush_ctx_->pending_segment_contexts_[segment.pending_segment_context_offset_].doc_id_end_
        = ctx.uncomitted_doc_id_begin_;
          segment.flush_ctx_->pending_segment_contexts_[segment.pending_segment_context_offset_].modification_offset_end_
        = ctx.uncomitted_modification_queries_;
        }
        */
      }

      if (flush_ctx && this != flush_ctx) {
        pending_segment_contexts_.pop_back();
        freelist_node = nullptr;
      }  // FIXME TODO remove this condition once col_writer tail is writen
         // correctly
    } else {
      // the segment is present in this flush_context
      // 'pending_segment_contexts_'
      assert(pending_segment_contexts_.size() >
             segment.pending_segment_context_offset_);
      assert(pending_segment_contexts_[segment.pending_segment_context_offset_]
               .segment_ == segment.ctx_);
      // +1 for the reference in 'pending_segment_contexts_', +1 for
      // the reference in 'active_segment_context'
      assert(pending_segment_contexts_[segment.pending_segment_context_offset_]
               .segment_.use_count() == 2);
      freelist_node =
        &(pending_segment_contexts_[segment.pending_segment_context_offset_]);
    }

    // NOTE: if the first uncommitted operation is a removal operation then it
    //       is fully valid for its 'committed' generation value to equal the
    //       generation of the last 'committed' insert operation since removals
    //       are applied to documents with generation <= removal
    assert(ctx.uncomitted_modification_queries_ <=
           ctx.modification_queries_.size());
    modification_count =
      ctx.modification_queries_.size() - ctx.uncomitted_modification_queries_;
    if (!generation_base) {
      if (flush_ctx && this != flush_ctx) {
        generation_base = flush_ctx->generation_ += modification_count;
      } else {
        // FIXME remove this condition once col_writer tail is writen correctly

        // atomic increment to end of unique generation range
        generation_base = generation_ += modification_count;
      }
      generation_base -= modification_count;  // start of generation range
    }
  }

  // noexcept state update operations below here
  // no need for segment lock since flush_all() operates on values < '*_end_'
  // update generation of segment operation

  // update generations of modification_queries_
  std::for_each(
    std::begin(ctx.modification_queries_) +
      ctx.uncomitted_modification_queries_,
    std::end(ctx.modification_queries_),
    [generation_base, modification_count](modification_context& v) noexcept {
      // must be < modification_count since inserts come after
      // modification
      assert(v.generation < modification_count);
      UNUSED(modification_count);

      // update to flush_context generation
      const_cast<size_t&>(v.generation) += generation_base;
    });

  auto update_generation = [uncomitted_doc_id_begin =
                              ctx.uncomitted_doc_id_begin_ - doc_limits::min(),
                            modification_count, generation_base](
                             std::span<segment_writer::update_context> ctxs) {
    // update generations of segment_context::flushed_update_contexts_
    for (size_t i = uncomitted_doc_id_begin, end = ctxs.size(); i < end; ++i) {
      // can == modification_count if inserts come  after
      // modification
      assert(ctxs[i].generation <= modification_count);
      UNUSED(modification_count);
      // update to flush_context generation
      ctxs[i].generation += generation_base;
    }
  };

  auto& flushed_update_contexts = ctx.flushed_update_contexts_;

  // update generations of segment_context::flushed_update_contexts_
  update_generation(flushed_update_contexts);

  assert(ctx.writer_);
  assert(ctx.writer_->docs_cached() <= doc_limits::eof());
  auto& writer = *(ctx.writer_);
  const auto writer_docs = writer.initialized() ? writer.docs_cached() : 0;

  // update generations of segment_writer::doc_contexts
  if (writer_docs) {
    update_generation(writer.docs_context());
  }

  // reset counters for segment reuse

  ctx.uncomitted_generation_offset_ = 0;
  ctx.uncomitted_doc_id_begin_ =
    flushed_update_contexts.size() + writer_docs + doc_limits::min();
  ctx.uncomitted_modification_queries_ = ctx.modification_queries_.size();

  if (!freelist_node) {
    // FIXME remove this condition once col_writer tail is writen correctly
    return;
  }

  // do not reuse segments that are present in another flush_context
  if (!ctx.dirty_) {
    assert(freelist_node);
    // +1 for 'active_segment_context::ctx_', +1 for
    assert(segment.ctx_.use_count() == 2);
    // 'pending_segment_context::segment_'
    auto& segments_active = *(segment.segments_active_);
    // release hold (delcare before aquisition since operator++() is noexcept)
    Finally segments_active_decrement = [&segments_active]() noexcept {
      --segments_active;
    };
    // increment counter to hold reservation while segment_context is being
    // released and added to the freelist
    ++segments_active;
    // reset before adding to freelist to garantee proper use_count() in
    // get_segment_context(...)
    segment = active_segment_context();
    // add segment_context to free-list
    pending_segment_contexts_freelist_.push(*freelist_node);
  }
}

void index_writer::flush_context::AddToPending(
  active_segment_context& segment) {
  if (segment.flush_ctx_ != nullptr) {
    // re-used active_segment_context
    return;
  }
  std::lock_guard lock{mutex_};
  auto const sizeBefore = pending_segment_contexts_.size();
  pending_segment_contexts_.emplace_back(segment.ctx_, sizeBefore);
  segment.flush_ctx_ = this;
  segment.pending_segment_context_offset_ = sizeBefore;
}

void index_writer::flush_context::reset() noexcept {
  // reset before returning to pool
  for (auto& entry : pending_segment_contexts_) {
    if (auto& segment = entry.segment_; segment.use_count() == 1) {
      // reset only if segment not tracked anywhere else
      segment->reset();
    }
  }

  // clear() before pending_segment_contexts_
  while (pending_segment_contexts_freelist_.pop())
    ;

  generation_.store(0);
  dir_->clear_refs();
  pending_segments_.clear();
  pending_segment_contexts_.clear();
  segment_mask_.clear();
}

index_writer::segment_context::segment_context(
  directory& dir, segment_meta_generator_t&& meta_generator,
  const column_info_provider_t& column_info,
  const feature_info_provider_t& feature_info, const comparer* comparator)
  : active_count_(0),
    buffered_docs_(0),
    dirty_(false),
    dir_(dir),
    meta_generator_(std::move(meta_generator)),
    uncomitted_doc_id_begin_(doc_limits::min()),
    uncomitted_generation_offset_(0),
    uncomitted_modification_queries_(0),
    writer_(segment_writer::make(dir_, column_info, feature_info, comparator)) {
  assert(meta_generator_);
}

uint64_t index_writer::segment_context::flush() {
  // must be already locked to prevent concurrent flush related modifications
  assert(!flush_mutex_.try_lock());

  if (!writer_ || !writer_->initialized() || !writer_->docs_cached()) {
    return 0;  // skip flushing an empty writer
  }

  assert(writer_->docs_cached() <= doc_limits::eof());

  auto& segment = flushed_.emplace_back(std::move(writer_meta_.meta));

  try {
    writer_->flush(segment);

    const std::span ctxs{writer_->docs_context()};
    flushed_update_contexts_.insert(flushed_update_contexts_.end(),
                                    ctxs.begin(), ctxs.end());
  } catch (...) {
    // failed to flush segment
    flushed_.pop_back();

    throw;
  }

  auto const tick = writer_->tick();
  writer_->reset();  // mark segment as already flushed
  return tick;
}

index_writer::segment_context::ptr index_writer::segment_context::make(
  directory& dir, segment_meta_generator_t&& meta_generator,
  const column_info_provider_t& column_info,
  const feature_info_provider_t& feature_info, const comparer* comparator) {
  return std::make_unique<segment_context>(
    dir, std::move(meta_generator), column_info, feature_info, comparator);
}

segment_writer::update_context
index_writer::segment_context::make_update_context(const filter& filter) {
  // increment generation due to removal
  auto generation = ++uncomitted_generation_offset_;
  auto update_id = modification_queries_.size();

  // -1 for previous generation
  modification_queries_.emplace_back(filter, generation - 1, true);

  return {generation, update_id};
}

segment_writer::update_context
index_writer::segment_context::make_update_context(
  std::shared_ptr<const filter> filter) {
  assert(filter);
  // increment generation due to removal
  auto generation = ++uncomitted_generation_offset_;
  auto update_id = modification_queries_.size();

  // -1 for previous generation
  modification_queries_.emplace_back(std::move(filter), generation - 1, true);

  return {generation, update_id};
}

segment_writer::update_context
index_writer::segment_context::make_update_context(filter::ptr&& filter) {
  assert(filter);
  // increment generation due to removal
  auto generation = ++uncomitted_generation_offset_;
  auto update_id = modification_queries_.size();

  // -1 for previous generation
  modification_queries_.emplace_back(std::move(filter), generation - 1, true);

  return {generation, update_id};
}

void index_writer::segment_context::prepare() {
  assert(writer_);

  if (!writer_->initialized()) {
    writer_meta_ = meta_generator_();
    writer_->reset(writer_meta_.meta);
  }
}

void index_writer::segment_context::remove(const filter& filter) {
  modification_queries_.emplace_back(filter, uncomitted_generation_offset_++,
                                     false);
}

void index_writer::segment_context::remove(
  std::shared_ptr<const filter> filter) {
  if (!filter) {
    return;  // skip empty filters
  }

  modification_queries_.emplace_back(std::move(filter),
                                     uncomitted_generation_offset_++, false);
}

void index_writer::segment_context::remove(filter::ptr&& filter) {
  if (!filter) {
    return;  // skip empty filters
  }

  modification_queries_.emplace_back(std::move(filter),
                                     uncomitted_generation_offset_++, false);
}

void index_writer::segment_context::reset(bool store_flushed) noexcept {
  active_count_.store(0);
  buffered_docs_.store(0);
  dirty_ = false;
  // in some cases we need to store flushed segments for further commits
  if (!store_flushed) {
    flushed_.clear();
    flushed_update_contexts_.clear();
  }
  modification_queries_.clear();
  uncomitted_doc_id_begin_ = doc_limits::min();
  uncomitted_generation_offset_ = 0;
  uncomitted_modification_queries_ = 0;

  if (writer_->initialized()) {
    writer_->reset();  // try to reduce number of files flushed below
  }

  // release refs only after clearing writer state to ensure
  // 'writer_' does not hold any files
  dir_.clear_refs();
}

index_writer::index_writer(
  ConstructToken, index_lock::ptr&& lock,
  index_file_refs::ref_t&& lock_file_ref, directory& dir, format::ptr codec,
  size_t segment_pool_size, const segment_options& segment_limits,
  const comparer* comparator, const column_info_provider_t& column_info,
  const feature_info_provider_t& feature_info,
  const payload_provider_t& meta_payload_provider, index_meta&& meta,
  committed_state_t&& committed_state)
  : feature_info_(feature_info),
    column_info_(column_info),
    meta_payload_provider_(meta_payload_provider),
    comparator_(comparator),
    cached_readers_(dir),
    codec_(codec),
    committed_state_(std::move(committed_state)),
    dir_(dir),
    // 2 because just swap them due to common commit lock
    flush_context_pool_(2),
    meta_(std::move(meta)),
    segment_limits_(segment_limits),
    segment_writer_pool_(segment_pool_size),
    segments_active_(0),
    writer_(codec->get_index_meta_writer()),
    write_lock_(std::move(lock)),
    write_lock_file_ref_(std::move(lock_file_ref)) {
  assert(column_info);   // ensured by 'make'
  assert(feature_info);  // ensured by 'make'
  assert(codec);
  flush_context_.store(&flush_context_pool_[0]);

  // setup round-robin chain
  for (size_t i = 0, count = flush_context_pool_.size() - 1; i < count; ++i) {
    flush_context_pool_[i].dir_ = std::make_unique<ref_tracking_directory>(dir);
    flush_context_pool_[i].next_context_ = &flush_context_pool_[i + 1];
  }

  // setup round-robin chain
  flush_context_pool_[flush_context_pool_.size() - 1].dir_ =
    std::make_unique<ref_tracking_directory>(dir);
  flush_context_pool_[flush_context_pool_.size() - 1].next_context_ =
    &flush_context_pool_[0];
}

void index_writer::clear(uint64_t tick) {
  // cppcheck-suppress unreadVariable
  std::lock_guard commit_lock{commit_lock_};

  if (!pending_state_ && meta_.empty() &&
      type_limits<type_t::index_gen_t>::valid(meta_.last_gen_)) {
    return;  // already empty
  }

  auto ctx = get_flush_context(false);
  // cppcheck-suppress unreadVariable
  // ensure there are no active struct update operations
  std::lock_guard ctx_lock{ctx->mutex_};

  auto pending_commit = std::make_shared<committed_state_t::element_type>(
    std::piecewise_construct,
    std::forward_as_tuple(std::make_shared<index_meta>()),
    std::forward_as_tuple());

  auto& dir = *ctx->dir_;
  auto& pending_meta = *pending_commit->first;

  // setup new meta
  pending_meta.update_generation(meta_);  // clone index metadata generation
  pending_meta.payload_buf_.clear();
  if (meta_payload_provider_ &&
      meta_payload_provider_(tick, pending_meta.payload_buf_)) {
    pending_meta.payload_ = pending_meta.payload_buf_;
  }
  pending_meta.seg_counter_.store(
    meta_.counter());  // ensure counter() >= max(seg#)

  // rollback already opened transaction if any
  writer_->rollback();

  // write 1st phase of index_meta transaction
  if (!writer_->prepare(dir, pending_meta)) {
    throw illegal_state{"Failed to write index metadata."};
  }

  auto ref =
    directory_utils::reference(dir, writer_->filename(pending_meta), true);
  if (ref) {
    auto& pending_refs = pending_commit->second;
    pending_refs.emplace_back(std::move(ref));
  }

  // 1st phase of the transaction successfully finished here
  // ensure new generation reflected in 'meta_'
  meta_.update_generation(pending_meta);
  pending_state_.ctx = std::move(ctx);  // retain flush context reference
  pending_state_.commit = std::move(pending_commit);

  finish();

  // all functions below are noexcept

  meta_.segments_.clear();  // noexcept op (clear after finish(), to match reset
                            // of pending_state_ inside finish(), allows
                            // recovery on clear() failure)
  cached_readers_.clear();  // original readers no longer required

  // clear consolidating segments
  // cppcheck-suppress unreadVariable
  std::lock_guard lock{consolidation_lock_};
  consolidating_segments_.clear();
}

index_writer::ptr index_writer::make(
  directory& dir, format::ptr codec, OpenMode mode,
  const init_options& opts /*= init_options()*/) {
  std::vector<index_file_refs::ref_t> file_refs;
  index_lock::ptr lock;
  index_file_refs::ref_t lockfile_ref;

  if (opts.lock_repository) {
    // lock the directory
    lock = dir.make_lock(kWriteLockName);
    // will be created by try_lock
    lockfile_ref = directory_utils::reference(dir, kWriteLockName, true);

    if (!lock || !lock->try_lock()) {
      throw lock_obtain_failed(kWriteLockName);
    }
  }

  // read from directory or create index metadata
  index_meta meta;
  {
    auto reader = codec->get_index_meta_reader();
    std::string segments_file;
    const bool index_exists = reader->last_segments_file(dir, segments_file);

    if (OM_CREATE == mode ||
        ((OM_CREATE | OM_APPEND) == mode && !index_exists)) {
      // Try to read. It allows us to
      // create writer against an index that's
      // currently opened for searching

      try {
        // for OM_CREATE meta must be fully recreated, meta read only to get
        // last version
        if (index_exists) {
          reader->read(dir, meta, segments_file);
          meta.clear();
          // this meta is for a totaly new index
          meta.last_gen_ = type_limits<type_t::index_gen_t>::invalid();
        }
      } catch (const error_base&) {
        meta = index_meta();
      }
    } else if (!index_exists) {
      throw file_not_found();  // no segments file found
    } else {
      reader->read(dir, meta, segments_file);
      append_segments_refs(file_refs, dir, meta);
      auto ref = directory_utils::reference(dir, segments_file);
      if (ref) {
        file_refs.emplace_back(std::move(ref));
      }
    }
  }

  auto comitted_state = std::make_shared<committed_state_t::element_type>(
    std::make_shared<index_meta>(meta), std::move(file_refs));

  auto writer = std::make_shared<index_writer>(
    ConstructToken{}, std::move(lock), std::move(lockfile_ref), dir, codec,
    opts.segment_pool_size, segment_options(opts), opts.comparator,
    opts.column_info ? opts.column_info : kDefaultColumnInfo,
    opts.features ? opts.features : kDefaultFeatureInfo,
    opts.meta_payload_provider, std::move(meta), std::move(comitted_state));

  // remove non-index files from directory
  directory_utils::remove_all_unreferenced(dir);

  return writer;
}

index_writer::~index_writer() noexcept {
  // failure may indicate a dangling 'document' instance
  assert(!segments_active_.load());
  cached_readers_.clear();
  write_lock_.reset();  // reset write lock if any
  // reset pending state (if any) before destroying flush contexts
  pending_state_.reset();
  flush_context_ = nullptr;
  // ensue all tracked segment_contexts are released before
  // segment_writer_pool_ is deallocated
  flush_context_pool_.clear();
}

uint64_t index_writer::buffered_docs() const {
  uint64_t docs_in_ram = 0;
  auto ctx = const_cast<index_writer*>(this)->get_flush_context();
  // 'pending_used_segment_contexts_'/'pending_free_segment_contexts_'
  // may be modified
  // cppcheck-suppress unreadVariable
  std::lock_guard lock{ctx->mutex_};

  for (auto& entry : ctx->pending_segment_contexts_) {
    // reading segment_writer::docs_count() is not thread safe
    // cppcheck-suppress useStlAlgorithm
    docs_in_ram += entry.segment_->buffered_docs_.load();
  }

  return docs_in_ram;
}

index_writer::consolidation_result index_writer::consolidate(
  const consolidation_policy_t& policy, format::ptr codec /*= nullptr*/,
  const merge_writer::flush_progress_t& progress /*= {}*/) {
  REGISTER_TIMER_DETAILED();
  if (!codec) {
    // use default codec if not specified
    codec = codec_;
  }

  consolidation_t candidates;
  const auto run_id = reinterpret_cast<size_t>(&candidates);

  // hold a reference to the last committed state to prevent files from being
  // deleted by a cleaner during the upcoming consolidation
  // use atomic_load(...) since finish() may modify the pointer
  auto committed_state = std::atomic_load(&committed_state_);
  assert(committed_state);
  if (IRS_UNLIKELY(!committed_state)) {
    return {0, ConsolidationError::FAIL};
  }
  auto committed_meta = committed_state->first;
  assert(committed_meta);
  if (IRS_UNLIKELY(!committed_meta)) {
    return {0, ConsolidationError::FAIL};
  }

  // collect a list of consolidation candidates
  {
    std::lock_guard lock{consolidation_lock_};
    // FIXME TODO remove from 'consolidating_segments_' any segments in
    // 'committed_state_' or 'pending_state_' to avoid data duplication
    policy(candidates, *committed_meta, consolidating_segments_);

    switch (candidates.size()) {
      case 0:
        // nothing to consolidate
        return {0, ConsolidationError::OK};
      case 1: {
        const auto* segment = *candidates.begin();

        if (!segment) {
          // invalid candidate
          return {0, ConsolidationError::FAIL};
        }

        if (segment->live_docs_count == segment->docs_count) {
          // no deletes, nothing to consolidate
          return {0, ConsolidationError::OK};
        }
      }
    }

    // check that candidates are not involved in ongoing merges
    for (const auto* candidate : candidates) {
      // segment has been already chosen for consolidation (or at least was
      // choosen), give up
      if (!candidate || consolidating_segments_.contains(candidate)) {
        return {0, ConsolidationError::FAIL};
      }
    }

    try {
      // register for consolidation
      consolidating_segments_.insert(candidates.begin(), candidates.end());
    } catch (...) {
      // rollback in case of insertion fails (finalizer below won`t handle
      // partial insert as concurrent consolidation is free to select same
      // candidate before finalizer reacquires the consolidation_lock)
      for (const auto* candidate : candidates) {
        consolidating_segments_.erase(candidate);
      }
      throw;
    }
  }

  // unregisterer for all registered candidates
  Finally unregister_segments = [&candidates, this]() noexcept {
    // FIXME make me noexcept as I'm begin called from within ~finally()
    if (candidates.empty()) {
      return;
    }
    std::lock_guard lock{consolidation_lock_};
    for (const auto* candidate : candidates) {
      consolidating_segments_.erase(candidate);
    }
  };

  // sort candidates
  std::sort(candidates.begin(), candidates.end());

  // remove duplicates
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());

  // validate candidates
  {
    size_t found = 0;

    for (const auto& segment : *committed_meta) {
      const auto it = std::lower_bound(std::begin(candidates),
                                       std::end(candidates), &segment.meta);
      found += (it != std::end(candidates) && *it == &segment.meta);
    }

    if (found != candidates.size()) {
      // not all candidates are valid
      IR_FRMT_DEBUG(
        "Failed to start consolidation for index generation "
        "'" IR_UINT64_T_SPECIFIER
        "', "
        "found only '" IR_SIZE_T_SPECIFIER "' out of '" IR_SIZE_T_SPECIFIER
        "' candidates",
        committed_meta->generation(), found, candidates.size());
      return {0, ConsolidationError::FAIL};
    }
  }

  IR_FRMT_TRACE("Starting consolidation id='" IR_SIZE_T_SPECIFIER "':\n%s",
                run_id, to_string(candidates).c_str());

  // do lock-free merge

  consolidation_result result{candidates.size(), ConsolidationError::FAIL};

  index_meta::index_segment_t consolidation_segment;
  consolidation_segment.meta.codec = codec;  // should use new codec
  consolidation_segment.meta.version = 0;    // reset version for new segment
  // increment active meta, not fn arg
  consolidation_segment.meta.name = file_name(meta_.increment());

  ref_tracking_directory dir(dir_);  // track references for new segment
  merge_writer merger(dir, column_info_, feature_info_, comparator_);
  merger.reserve(result.size);

  // add consolidated segments to the merge_writer
  for (const auto* segment : candidates) {
    // already checked validity
    assert(segment);

    auto reader = cached_readers_.emplace(*segment);

    if (reader) {
      // merge_writer holds a reference to reader
      merger.add(static_cast<sub_reader::ptr>(reader));
    }
  }

  // we do not persist segment meta since some removals may come later
  if (!merger.flush(consolidation_segment, progress)) {
    // nothing to consolidate or consolidation failure
    return result;
  }

  // commit merge
  {
    // ensure committed_state_ segments are not modified by concurrent
    // consolidate()/commit()
    std::unique_lock lock{commit_lock_};
    const auto current_committed_meta = committed_state_->first;
    assert(current_committed_meta);
    if (IRS_UNLIKELY(!current_committed_meta)) {
      return {0, ConsolidationError::FAIL};
    }

    auto cleanup_cached_readers = [&current_committed_meta, &candidates,
                                   this]() noexcept {
      // FIXME make me noexcept as I'm begin called from within ~finally()
      if (!candidates.empty()) {
        decltype(flush_context::segment_mask_) cached_mask;
        // pointers are different so check by name
        for (const auto* candidate : candidates) {
          if (current_committed_meta->end() ==
              std::find_if(current_committed_meta->begin(),
                           current_committed_meta->end(),
                           [&candidate](const index_meta::index_segment_t& s) {
                             return candidate->name == s.meta.name;
                           })) {
            // found missing segment. Mask it!
            cached_mask.insert(*candidate);
          }
        }
        if (!cached_mask.empty()) {
          cached_readers_.purge(cached_mask);
        }
      }
    };

    if (pending_state_) {
      // we could possibly need cleanup
      Finally unregister_missing_cached_readers =
        std::move(cleanup_cached_readers);

      // check we didn`t added to reader cache already absent readers
      // only if we have different index meta
      if (committed_meta != current_committed_meta) {
        // pointers are different so check by name
        for (const auto* candidate : candidates) {
          if (current_committed_meta->end() ==
              std::find_if(current_committed_meta->begin(),
                           current_committed_meta->end(),
                           [&candidate](const index_meta::index_segment_t& s) {
                             return candidate->name == s.meta.name;
                           })) {
            // not all candidates are valid
            IR_FRMT_DEBUG(
              "Failed to start consolidation for index generation "
              "'" IR_UINT64_T_SPECIFIER
              "', not found segment %s in committed state",
              committed_meta->generation(), candidate->name.c_str());
            return result;
          }
        }
      }

      result.error = ConsolidationError::PENDING;

      // transaction has been started, we're somewhere in the middle

      // can modify ctx->segment_mask_ without
      // lock since have commit_lock_
      auto ctx = get_flush_context();

      // register consolidation for the next transaction
      ctx->pending_segments_.emplace_back(
        std::move(consolidation_segment),
        std::numeric_limits<size_t>::max(),  // skip deletes, will accumulate
                                             // deletes from existing candidates
        extract_refs(dir),                   // do not forget to track refs
        std::move(candidates),               // consolidation context candidates
        std::move(committed_meta),           // consolidation context meta
        std::move(merger));                  // merge context

      IR_FRMT_TRACE("Consolidation id='" IR_SIZE_T_SPECIFIER
                    "' successfully finished: pending",
                    run_id);
    } else if (committed_meta == current_committed_meta) {
      // before new transaction was started:
      // no commits happened in since consolidation was started

      auto ctx = get_flush_context();
      // lock due to context modification
      std::lock_guard ctx_lock{ctx->mutex_};

      // can release commit lock, we guarded against commit by
      // locked flush context
      lock.unlock();

      auto& segment_mask = ctx->segment_mask_;

      // persist segment meta
      index_utils::flush_index_segment(dir, consolidation_segment);
      segment_mask.reserve(segment_mask.size() + candidates.size());
      ctx->pending_segments_.emplace_back(
        std::move(consolidation_segment),
        0,  // deletes must be applied to the consolidated segment
        extract_refs(dir),         // do not forget to track refs
        std::move(candidates),     // consolidation context candidates
        std::move(committed_meta)  // consolidation context meta
      );

      // filter out merged segments for the next commit
      const auto& pending_segment = ctx->pending_segments_.back();
      const auto& consolidation_ctx = pending_segment.consolidation_ctx;
      const auto& consolidation_meta = pending_segment.segment.meta;

      // mask mapped candidates
      // segments from the to-be added new segment
      for (const auto* segment : consolidation_ctx.candidates) {
        segment_mask.emplace(*segment);
      }

      IR_FRMT_TRACE(
        "Consolidation id='" IR_SIZE_T_SPECIFIER
        "' successfully finished: "
        "Name='%s', docs_count=" IR_UINT64_T_SPECIFIER
        ", "
        "live_docs_count=" IR_UINT64_T_SPECIFIER
        ", "
        "size=" IR_SIZE_T_SPECIFIER "",
        run_id, consolidation_meta.name.c_str(), consolidation_meta.docs_count,
        consolidation_meta.live_docs_count, consolidation_meta.size);
    } else {
      // before new transaction was started:
      // there was a commit(s) since consolidation was started,

      // we could possibly need cleanup
      Finally unregister_missing_cached_readers =
        std::move(cleanup_cached_readers);

      auto ctx = get_flush_context();
      // lock due to context modification
      std::lock_guard ctx_lock{ctx->mutex_};

      // can release commit lock, we guarded against commit by
      // locked flush context
      lock.unlock();

      auto& segment_mask = ctx->segment_mask_;

      candidates_mapping_t mappings;
      const auto res = map_candidates(mappings, candidates,
                                      current_committed_meta->segments());

      if (res.second != candidates.size()) {
        // at least one candidate is missing
        // can't finish consolidation
        IR_FRMT_DEBUG("Failed to finish consolidation id='" IR_SIZE_T_SPECIFIER
                      "' for segment '%s', "
                      "found only '" IR_SIZE_T_SPECIFIER
                      "' out of '" IR_SIZE_T_SPECIFIER "' candidates",
                      run_id, consolidation_segment.meta.name.c_str(),
                      res.second, candidates.size());

        return result;
      }

      // handle deletes if something changed
      if (res.first) {
        document_mask docs_mask;

        if (!map_removals(mappings, merger, cached_readers_, docs_mask)) {
          // consolidated segment has docs missing from
          // current_committed_meta->segments()
          IR_FRMT_DEBUG(
            "Failed to finish consolidation id='" IR_SIZE_T_SPECIFIER
            "' for segment '%s', "
            "due removed documents still present the consolidation candidates",
            run_id, consolidation_segment.meta.name.c_str());

          return result;
        }

        if (!docs_mask.empty()) {
          consolidation_segment.meta.live_docs_count -= docs_mask.size();
          write_document_mask(dir, consolidation_segment.meta, docs_mask,
                              false);
        }
      }

      // persist segment meta
      index_utils::flush_index_segment(dir, consolidation_segment);
      segment_mask.reserve(segment_mask.size() + candidates.size());
      ctx->pending_segments_.emplace_back(
        std::move(consolidation_segment),
        0,  // deletes must be applied to the consolidated segment
        extract_refs(dir),           // do not forget to track refs
        std::move(candidates),       // consolidation context candidates
        std::move(committed_meta));  // consolidation context meta

      // filter out merged segments for the next commit
      const auto& pending_segment = ctx->pending_segments_.back();
      const auto& consolidation_ctx = pending_segment.consolidation_ctx;
      const auto& consolidation_meta = pending_segment.segment.meta;

      // mask mapped candidates
      // segments from the to-be added new segment
      for (const auto* segment : consolidation_ctx.candidates) {
        segment_mask.emplace(*segment);
      }

      // mask mapped (matched) segments
      // segments from the already finished commit
      for (auto& segment : current_committed_meta->segments()) {
        if (mappings.contains(segment.meta.name)) {
          segment_mask.emplace(segment.meta);
        }
      }

      IR_FRMT_TRACE(
        "Consolidation id='" IR_SIZE_T_SPECIFIER
        "' successfully finished:\nName='%s', "
        "docs_count=" IR_UINT64_T_SPECIFIER
        ", "
        "live_docs_count=" IR_UINT64_T_SPECIFIER
        ", "
        "size=" IR_SIZE_T_SPECIFIER "",
        run_id, consolidation_meta.name.c_str(), consolidation_meta.docs_count,
        consolidation_meta.live_docs_count, consolidation_meta.size);
    }
  }

  result.error = ConsolidationError::OK;
  return result;
}

bool index_writer::import(
  const index_reader& reader, format::ptr codec /*= nullptr*/,
  const merge_writer::flush_progress_t& progress /*= {}*/) {
  if (!reader.live_docs_count()) {
    return true;  // skip empty readers since no documents to import
  }

  if (!codec) {
    codec = codec_;
  }

  ref_tracking_directory dir(dir_);  // track references

  index_meta::index_segment_t segment;
  segment.meta.name = file_name(meta_.increment());
  segment.meta.codec = codec;

  merge_writer merger(dir, column_info_, feature_info_, comparator_);
  merger.reserve(reader.size());

  for (auto& curr_segment : reader) {
    merger.add(curr_segment);
  }

  if (!merger.flush(segment, progress)) {
    return false;  // import failure (no files created, nothing to clean up)
  }

  index_utils::flush_index_segment(dir, segment);

  auto refs = extract_refs(dir);

  auto ctx = get_flush_context();
  // lock due to context modification
  // cppcheck-suppress unreadVariable
  std::lock_guard lock{ctx->mutex_};

  ctx->pending_segments_.emplace_back(
    std::move(segment),
    ctx->generation_.load(),  // current modification generation
    std::move(refs)           // do not forget to track refs
  );

  return true;
}

index_writer::flush_context_ptr index_writer::get_flush_context(
  bool shared /*= true*/) {
  auto* ctx = flush_context_.load();  // get current ctx

  if (!shared) {
    for (;;) {
      // lock ctx exchange (write-lock)
      std::unique_lock lock{ctx->flush_mutex_};

      // aquire the current flush_context and its lock
      if (!flush_context_.compare_exchange_strong(ctx, ctx->next_context_)) {
        ctx = flush_context_.load();  // it might have changed
        continue;
      }

      lock.release();

      return {ctx, [](flush_context* ctx) noexcept -> void {
                std::unique_lock lock{ctx->flush_mutex_, std::adopt_lock};
                // reset context and make ready for reuse
                ctx->reset();
              }};
    }
  }

  for (;;) {
    // lock current ctx (read-lock)
    std::shared_lock lock{ctx->flush_mutex_, std::try_to_lock};

    if (!lock) {
      std::this_thread::yield();    // allow flushing thread to finish exchange
      ctx = flush_context_.load();  // it might have changed
      continue;
    }

    // at this point flush_context_ might have already changed
    // get active ctx, since initial_ctx is locked it will never be swapped with
    // current until unlocked
    auto* flush_ctx = flush_context_.load();

    // primary_flush_context_ has changed
    if (ctx != flush_ctx) {
      ctx = flush_ctx;
      continue;
    }

    lock.release();

    return {ctx, [](flush_context* ctx) noexcept -> void {
              std::shared_lock lock{ctx->flush_mutex_, std::adopt_lock};
            }};
  }
}

index_writer::active_segment_context index_writer::get_segment_context(
  flush_context& ctx) {
  // release reservation (delcare before aquisition since operator++() is
  // noexcept)
  Finally segments_active_decrement = [this]() noexcept { --segments_active_; };
  // increment counter to aquire reservation, if another thread
  // tries to reserve last context then it'll be over limit
  auto segments_active = ++segments_active_;
  auto segment_count_max = segment_limits_.segment_count_max.load();

  // no free segment_context available and maximum number of segments reached
  // must return to caller so as to unlock/relock flush_context before retrying
  // to get a new segment so as to avoid a deadlock due to a read-write-read
  // situation for flush_context::flush_mutex_ with threads trying to lock
  // flush_context::flush_mutex_ to return their segment_context
  if (segment_count_max &&
      // '<' to account for +1 reservation
      segment_count_max < segments_active) {
    return active_segment_context();
  }

  // only nodes of type 'pending_segment_context' are added to
  // 'pending_segment_contexts_freelist_'
  auto* freelist_node = static_cast<flush_context::pending_segment_context*>(
    ctx.pending_segment_contexts_freelist_.pop());

  if (freelist_node) {
    const auto& segment = freelist_node->segment_;

    // +1 for the reference in 'pending_segment_contexts_'
    assert(segment.use_count() == 1);
    assert(!segment->dirty_);
    return active_segment_context(segment, segments_active_, &ctx,
                                  freelist_node->value);
  }

  // should allocate a new segment_context from the pool

  auto meta_generator = [this]() -> segment_meta {
    return segment_meta(file_name(meta_.increment()), codec_);
  };

  std::shared_ptr<segment_context> segment_ctx{segment_writer_pool_.emplace(
    dir_, std::move(meta_generator), column_info_, feature_info_, comparator_)};
  auto segment_memory_max = segment_limits_.segment_memory_max.load();

  // recreate writer if it reserved more memory than allowed by current limits
  if (segment_memory_max &&
      segment_memory_max < segment_ctx->writer_->memory_reserved()) {
    segment_ctx->writer_ = segment_writer::make(segment_ctx->dir_, column_info_,
                                                feature_info_, comparator_);
  }

  return active_segment_context(segment_ctx, segments_active_);
}

std::pair<std::vector<std::unique_lock<std::mutex>>, uint64_t>
index_writer::flush_pending(flush_context& ctx,
                            std::unique_lock<std::mutex>& ctx_lock) {
  uint64_t max_tick = 0;
  std::vector<std::unique_lock<std::mutex>> segment_flush_locks;
  segment_flush_locks.reserve(ctx.pending_segment_contexts_.size());

  for (auto& entry : ctx.pending_segment_contexts_) {
    auto& segment = entry.segment_;

    // mark the 'segment_context' as dirty so that it will not be reused if this
    // 'flush_context' once again becomes the active context while the
    // 'segment_context' handle is still held by documents()
    segment->dirty_ = true;

    // wait for the segment to no longer be active
    // i.e. wait for all ongoing document operations to finish (insert/replace)
    // the segment will not be given out again by the active 'flush_context'
    // because it was started by a different 'flush_context', i.e. by 'ctx'

    // FIXME remove this condition once col_writer tail is writen correctly
    while (segment->active_count_.load() || segment.use_count() != 1) {
      // arbitrary sleep interval
      ctx.pending_segment_context_cond_.wait_for(ctx_lock, 50ms);
    }

    // prevent concurrent modification of segment_context properties during
    // flush_context::emplace(...)
    // FIXME flush_all() blocks flush_context::emplace(...) and
    // insert()/remove()/replace()
    segment_flush_locks.emplace_back(segment->flush_mutex_);

    // force a flush of the underlying segment_writer
    max_tick = std::max(segment->flush(), max_tick);

    // may be std::numeric_limits<size_t>::max() if segment_meta only in this
    // flush_context
    entry.doc_id_end_ =
      std::min(segment->uncomitted_doc_id_begin_, entry.doc_id_end_);
    entry.modification_offset_end_ =
      std::min(segment->uncomitted_modification_queries_,
               entry.modification_offset_end_);
  }

  return {std::move(segment_flush_locks), max_tick};
}

index_writer::pending_context_t index_writer::flush_all(
  progress_report_callback const& progress_callback) {
  REGISTER_TIMER_DETAILED();

  auto const& progress =
    (progress_callback != nullptr ? progress_callback : kNoProgress);

  bool modified = !type_limits<type_t::index_gen_t>::valid(meta_.last_gen_);
  sync_context to_sync;
  document_mask docs_mask;

  auto pending_meta = std::make_unique<index_meta>();
  auto& segments = pending_meta->segments_;

  auto ctx = get_flush_context(false);
  auto& dir = *(ctx->dir_);
  // ensure there are no active struct update operations
  std::unique_lock lock{ctx->mutex_};

  // register consolidating segments cleanup.
  // we need raw ptr as ctx may be moved
  Finally unregister_segments = [ctx_raw = ctx.get(), this]() noexcept {
    // FIXME make me noexcept as I'm begin called from within ~finally()
    assert(ctx_raw);
    if (ctx_raw->pending_segments_.empty()) {
      return;
    }
    std::lock_guard lock{consolidation_lock_};

    for (auto& pending_segment : ctx_raw->pending_segments_) {
      auto& candidates = pending_segment.consolidation_ctx.candidates;
      for (const auto* candidate : candidates) {
        consolidating_segments_.erase(candidate);
      }
    }
  };

  // Stage 0
  // wait for any outstanding segments to settle to ensure that any rollbacks
  // are properly tracked in 'modification_queries_'

  const auto [segment_flush_locks, max_tick] = flush_pending(*ctx, lock);

  // Stage 1
  // update document_mask for existing (i.e. sealed) segments

  auto& segment_mask = ctx->segment_mask_;

  // only used for progress reporting
  size_t current_segment_index = 0;

  for (auto& existing_segment : meta_) {
    // report progress
    progress("Stage 1: Apply removals to the existing segments",
             current_segment_index, meta_.size());
    ++current_segment_index;

    // skip already masked segments
    if (segment_mask.contains(existing_segment.meta)) {
      continue;
    }

    const auto segment_id = segments.size();
    segments.emplace_back(existing_segment);

    auto mask_modified = false;
    auto& segment = segments.back();

    docs_mask.clear();
    index_utils::read_document_mask(docs_mask, dir, segment.meta);

    // mask documents matching filters from segment_contexts (i.e. from new
    // operations)
    for (auto& modifications : ctx->pending_segment_contexts_) {
      // modification_queries_ range
      // [flush_segment_context::modification_offset_begin_,
      // segment_context::uncomitted_modification_queries_)
      auto modifications_begin = modifications.modification_offset_begin_;
      auto modifications_end = modifications.modification_offset_end_;

      assert(modifications_begin <= modifications_end);
      assert(modifications_end <=
             modifications.segment_->modification_queries_.size());
      const std::span modification_queries{
        modifications.segment_->modification_queries_.data() +
          modifications_begin,
        modifications_end - modifications_begin};

      mask_modified |= add_document_mask_modified_records(
        modification_queries, docs_mask, cached_readers_, segment.meta);
    }

    ++current_segment_index;

    // write docs_mask if masks added, if all docs are masked then mask segment
    if (mask_modified) {
      // mask empty segments
      if (!segment.meta.live_docs_count) {
        // mask segment to clear reader cache
        segment_mask.emplace(existing_segment.meta);
        // remove empty segment
        segments.pop_back();
        // removal of one of the existing segments
        modified = true;
        continue;
      }

      // mask segment since write_document_mask(...) will increment version
      segment_mask.emplace(existing_segment.meta);
      to_sync.register_partial_sync(
        segment_id, write_document_mask(dir, segment.meta, docs_mask));
      segment.meta.size = 0;                           // reset for new write
      index_utils::flush_index_segment(dir, segment);  // write with new mask
    }
  }

  // Stage 2
  // add pending complete segments registered by import or consolidation

  // number of candidates that have been registered for
  // pending consolidation
  size_t current_pending_segments_index = 0;
  size_t pending_candidates_count = 0;
  for (auto& pending_segment : ctx->pending_segments_) {
    // report progress
    progress("Stage 2: Handling consolidated/imported segments",
             current_pending_segments_index, ctx->pending_segments_.size());
    ++current_pending_segments_index;

    // pending consolidation
    auto& candidates = pending_segment.consolidation_ctx.candidates;
    docs_mask.clear();
    bool pending_consolidation = pending_segment.consolidation_ctx.merger;
    if (pending_consolidation) {
      // pending consolidation request
      candidates_mapping_t mappings;
      const auto res = map_candidates(mappings, candidates, segments);

      if (res.second != candidates.size()) {
        // at least one candidate is missing
        // in pending meta can't finish consolidation
        IR_FRMT_DEBUG(
          "Failed to finish merge for segment '%s', found only "
          "'" IR_SIZE_T_SPECIFIER "' out of '" IR_SIZE_T_SPECIFIER
          "' candidates",
          pending_segment.segment.meta.name.c_str(), res.second,
          candidates.size());

        continue;  // skip this particular consolidation
      }

      // mask mapped candidates
      // segments from the to-be added new segment
      for (auto& mapping : mappings) {
        ctx->segment_mask_.emplace(*(mapping.second.second.first));
      }

      // mask mapped (matched) segments
      // segments from the currently ongoing commit
      for (auto& segment : segments) {
        if (mappings.contains(segment.meta.name)) {
          ctx->segment_mask_.emplace(segment.meta);
        }
      }

      // have some changes, apply deletes
      if (res.first) {
        auto success =
          map_removals(mappings, pending_segment.consolidation_ctx.merger,
                       cached_readers_, docs_mask);

        if (!success) {
          // consolidated segment has docs missing from 'segments'
          IR_FRMT_WARN(
            "Failed to finish merge for segment '%s', due removed documents "
            "still present the consolidation candidates",
            pending_segment.segment.meta.name.c_str());

          continue;  // skip this particular consolidation
        }
      }

      // we're done with removals for pending consolidation
      // they have been already applied to candidates above
      // and succesfully remapped to consolidated segment
      pending_segment.segment.meta.live_docs_count -= docs_mask.size();

      // we've seen at least 1 successfully applied
      // pending consolidation request
      pending_candidates_count += candidates.size();
    } else {
      // during consolidation doc_mask could be already populated even for just
      // merged segment
      if (pending_segment.segment.meta.docs_count !=
          pending_segment.segment.meta.live_docs_count) {
        index_utils::read_document_mask(docs_mask, dir,
                                        pending_segment.segment.meta);
      }
      bool docs_mask_modified = false;
      // pending already imported/consolidated segment, apply deletes
      // mask documents matching filters from segment_contexts (i.e. from new
      // operations)
      for (auto& modifications : ctx->pending_segment_contexts_) {
        // modification_queries_ range
        // [flush_segment_context::modification_offset_begin_,
        // segment_context::uncomitted_modification_queries_)
        auto modifications_begin = modifications.modification_offset_begin_;
        auto modifications_end = modifications.modification_offset_end_;

        assert(modifications_begin <= modifications_end);
        assert(modifications_end <=
               modifications.segment_->modification_queries_.size());
        const std::span modification_queries{
          modifications.segment_->modification_queries_.data() +
            modifications_begin,
          modifications_end - modifications_begin};

        docs_mask_modified |= add_document_mask_modified_records(
          modification_queries, docs_mask, cached_readers_,
          pending_segment.segment.meta, pending_segment.generation);
      }

      // if mask left untouched, reset it, to prevent unnecessary writes
      if (!docs_mask_modified) {
        docs_mask.clear();
      }
    }

    // skip empty segments
    if (!pending_segment.segment.meta.live_docs_count) {
      ctx->segment_mask_.emplace(pending_segment.segment.meta);
      modified = true;
      continue;
    }

    // write non-empty document mask
    if (!docs_mask.empty()) {
      if (!pending_consolidation) {
        // if this is pending consolidation,
        // this segment is already in the mask (see assert below)
        // new version will be created. Remove old version from cache!
        ctx->segment_mask_.emplace(pending_segment.segment.meta);
      }
      write_document_mask(dir, pending_segment.segment.meta, docs_mask,
                          !pending_consolidation);
      pending_consolidation = true;  // force write new segment meta
    }

    // persist segment meta
    if (pending_consolidation) {
      index_utils::flush_index_segment(dir, pending_segment.segment);
    }

    // register full segment sync
    to_sync.register_full_sync(segments.size());
    segments.emplace_back(std::move(pending_segment.segment));
  }

  if (pending_candidates_count) {
    // for pending consolidation we need to filter out
    // consolidation candidates after applying them
    index_meta::index_segments_t tmp;
    decltype(sync_context::segments) tmp_sync;

    tmp.reserve(segments.size() - pending_candidates_count);
    tmp_sync.reserve(to_sync.segments.size());

    auto begin = to_sync.segments.begin();
    auto end = to_sync.segments.end();

    for (size_t i = 0, size = segments.size(); i < size; ++i) {
      auto& segment = segments[i];

      // valid segment
      const bool valid = !ctx->segment_mask_.contains(segment.meta);

      if (begin != end && i == begin->first) {
        if (valid) {
          tmp_sync.emplace_back(tmp.size(), begin->second);
        }

        ++begin;
      }

      if (valid) {
        tmp.emplace_back(std::move(segment));
      }
    }

    segments = std::move(tmp);
    to_sync.segments = std::move(tmp_sync);
  }

  // Stage 3
  // create new segments

  {
    // count total number of segments once
    size_t total_pending_segment_context_segments = 0;
    for (auto& pending_segment_context : ctx->pending_segment_contexts_) {
      if (auto& segment = pending_segment_context.segment_; segment) {
        total_pending_segment_context_segments += segment->flushed_.size();
      }
    }

    std::vector<flush_segment_context> segment_ctxs;
    size_t current_pending_segment_context_segments = 0;

    // proces all segments that have been seen by the current flush_context
    for (auto& pending_segment_context : ctx->pending_segment_contexts_) {
      auto& segment = pending_segment_context.segment_;
      if (!segment) {
        continue;  // skip empty segments
      }

      size_t flushed_docs_count = 0;
      // was updated after flush
      auto flushed_doc_id_end = pending_segment_context.doc_id_end_;
      assert(pending_segment_context.doc_id_begin_ <= flushed_doc_id_end);
      assert(flushed_doc_id_end - doc_limits::min() <=
             segment->flushed_update_contexts_.size());

      // process individually each flushed segment_meta from the segment_context
      for (auto& flushed : segment->flushed_) {
        // report progress
        progress("Stage 3: Creating new segments",
                 current_pending_segment_context_segments,
                 total_pending_segment_context_segments);
        ++current_pending_segment_context_segments;

        auto flushed_docs_start = flushed_docs_count;

        // sum of all previous segment_meta::docs_count including this meta
        flushed_docs_count += flushed.meta.docs_count;

        if (!flushed.meta.live_docs_count /* empty segment_meta */
            // segment_meta fully before the start of this flush_context
            || flushed_doc_id_end - doc_limits::min() <= flushed_docs_start
            // segment_meta fully after the start of this flush_context
            || pending_segment_context.doc_id_begin_ - doc_limits::min() >=
                 flushed_docs_count) {
          continue;
        }

        // 0-based
        auto update_contexts_begin =
          std::max(pending_segment_context.doc_id_begin_ - doc_limits::min(),
                   flushed_docs_start);
        // 0-based
        auto update_contexts_end =
          std::min(flushed_doc_id_end - doc_limits::min(), flushed_docs_count);
        assert(update_contexts_begin <= update_contexts_end);
        // begining doc_id in this segment_meta
        auto valid_doc_id_begin =
          update_contexts_begin - flushed_docs_start + doc_limits::min();
        auto valid_doc_id_end =
          std::min(update_contexts_end - flushed_docs_start + doc_limits::min(),
                   size_t(flushed.docs_mask_tail_doc_id));
        assert(valid_doc_id_begin <= valid_doc_id_end);

        if (valid_doc_id_begin == valid_doc_id_end) {
          continue;  // empty segment since head+tail == 'docs_count'
        }

        const std::span flush_update_contexts{
          segment->flushed_update_contexts_.data() + flushed_docs_start,
          flushed.meta.docs_count};

        segment_ctxs.emplace_back(flushed, valid_doc_id_begin, valid_doc_id_end,
                                  flush_update_contexts,
                                  segment->modification_queries_);

        // increment version for next run due to documents masked
        // from this run, similar to write_document_mask(...)
        ++flushed.meta.version;

        auto& flush_segment_ctx = segment_ctxs.back();

        // read document_mask as was originally flushed
        // could be due to truncated records due to rollback of uncommitted data
        index_utils::read_document_mask(flush_segment_ctx.docs_mask_,
                                        segment->dir_,
                                        flush_segment_ctx.segment_.meta);

        // add doc_ids before start of this flush_context to document_mask
        for (size_t doc_id = doc_limits::min(); doc_id < valid_doc_id_begin;
             ++doc_id) {
          assert(std::numeric_limits<doc_id_t>::max() >= doc_id);
          if (flush_segment_ctx.docs_mask_.emplace(doc_id_t(doc_id)).second) {
            assert(flush_segment_ctx.segment_.meta.live_docs_count);
            // decrement count of live docs
            --flush_segment_ctx.segment_.meta.live_docs_count;
          }
        }

        // add tail doc_ids not part of this flush_context to documents_mask
        // (including truncated)
        for (size_t doc_id = valid_doc_id_end,
                    doc_id_end = flushed.meta.docs_count + doc_limits::min();
             doc_id < doc_id_end; ++doc_id) {
          assert(std::numeric_limits<doc_id_t>::max() >= doc_id);
          if (flush_segment_ctx.docs_mask_.emplace(doc_id_t(doc_id)).second) {
            assert(flush_segment_ctx.segment_.meta.live_docs_count);
            // decrement count of live docs
            --flush_segment_ctx.segment_.meta.live_docs_count;
          }
        }

        bool segment_modified{false};
        // mask documents matching filters from all flushed segment_contexts
        // (i.e. from new operations)
        for (auto& modifications : ctx->pending_segment_contexts_) {
          auto modifications_begin = modifications.modification_offset_begin_;
          auto modifications_end = modifications.modification_offset_end_;

          assert(modifications_begin <= modifications_end);
          assert(modifications_end <=
                 modifications.segment_->modification_queries_.size());
          const std::span modification_queries(
            modifications.segment_->modification_queries_.data() +
              modifications_begin,
            modifications_end - modifications_begin);

          segment_modified |= add_document_mask_modified_records(
            modification_queries, flush_segment_ctx, cached_readers_);
        }
        if (segment_modified) {
          ctx->segment_mask_.emplace(flush_segment_ctx.segment_.meta);
        }
      }
    }

    // write docs_mask if !empty(), if all docs are masked then remove segment
    // altogether
    size_t current_segment_ctxs = 0;
    for (auto& segment_ctx : segment_ctxs) {
      // report progress - note: from the code, we are still a part of stage 3,
      // but we need to report something different here, i.e. "stage 4"
      progress("Stage 4: Applying removals for new segmets",
               current_segment_ctxs, segment_ctxs.size());
      ++current_segment_ctxs;

      // if have a writer with potential update-replacement records then check
      // if they were seen
      add_document_mask_unused_updates(segment_ctx);

      // after mismatched replaces here could be also empty segment
      // so masking is needed
      if (!segment_ctx.segment_.meta.live_docs_count) {
        ctx->segment_mask_.emplace(segment_ctx.segment_.meta);
        continue;
      }
      // write non-empty document mask
      if (!segment_ctx.docs_mask_.empty()) {
        write_document_mask(dir, segment_ctx.segment_.meta,
                            segment_ctx.docs_mask_);
        // write with new mask
        index_utils::flush_index_segment(dir, segment_ctx.segment_);
      }

      // register full segment sync
      to_sync.register_full_sync(segments.size());
      segments.emplace_back(std::move(segment_ctx.segment_));
    }
  }

  pending_meta->update_generation(meta_);  // clone index metadata generation

  modified |= !to_sync.empty();

  // only flush a new index version upon a new index or a metadata change
  if (!modified) {
    // even if nothing to commit, we may have populated readers cache! Need to
    // cleanup.
    if (!ctx->segment_mask_.empty()) {
      cached_readers_.purge(ctx->segment_mask_);
    }
    return pending_context_t();
  }

  pending_meta->payload_buf_.clear();
  if (meta_payload_provider_ &&
      meta_payload_provider_(max_tick, pending_meta->payload_buf_)) {
    pending_meta->payload_ = pending_meta->payload_buf_;
  }

  pending_meta->seg_counter_.store(
    meta_.counter());  // ensure counter() >= max(seg#)

  pending_context_t pending_context;
  pending_context.ctx = std::move(ctx);  // retain flush context reference
  pending_context.meta = std::move(pending_meta);  // retain meta pending flush
  pending_context.to_sync = std::move(to_sync);

  return pending_context;
}

bool index_writer::start(progress_report_callback const& progress) {
  assert(!commit_lock_.try_lock());  // already locked

  REGISTER_TIMER_DETAILED();

  if (pending_state_) {
    // begin has been already called
    // without corresponding call to commit
    return false;
  }

  auto to_commit = flush_all(progress);

  if (!to_commit) {
    // nothing to commit, no transaction started
    return false;
  }

  auto& dir = *to_commit.ctx->dir_;
  auto& pending_meta = *to_commit.meta;

  // write 1st phase of index_meta transaction
  if (!writer_->prepare(dir, pending_meta)) {
    throw illegal_state{"Failed to write index metadata."};
  }

  Finally update_generation = [this, &pending_meta]() noexcept {
    meta_.update_generation(pending_meta);
  };

  files_to_sync_.clear();
  auto sync = [this](std::string_view file) {
    files_to_sync_.emplace_back(file);
    return true;
  };

  try {
    // sync all pending files
    to_commit.to_sync.visit(sync, pending_meta);

    if (!dir.sync(files_to_sync_)) {
      throw io_error("Failed to sync files.");
    }

    // track all refs
    file_refs_t pending_refs;
    append_segments_refs(pending_refs, dir, pending_meta);
    auto ref =
      directory_utils::reference(dir, writer_->filename(pending_meta), true);
    if (ref) {
      pending_refs.emplace_back(std::move(ref));
    }

    meta_.segments_ = to_commit.meta->segments_;  // create copy

    // 1st phase of the transaction successfully finished here,
    // set to_commit as active flush context containing pending meta
    pending_state_.commit = std::make_shared<committed_state_t::element_type>(
      std::piecewise_construct,
      std::forward_as_tuple(std::move(to_commit.meta)),
      std::forward_as_tuple(std::move(pending_refs)));
  } catch (...) {
    writer_->rollback();  // rollback started transaction

    throw;
  }

  // only noexcept operations below

  // release cached readers
  cached_readers_.purge(to_commit.ctx->segment_mask_);
  pending_state_.ctx = std::move(to_commit.ctx);

  return true;
}

void index_writer::finish() {
  assert(!commit_lock_.try_lock());  // already locked

  REGISTER_TIMER_DETAILED();

  if (!pending_state_) {
    return;
  }

  Finally reset_state = [this]() noexcept {
    // release reference to flush_context
    pending_state_.reset();
  };

  // lightweight 2nd phase of the transaction

  try {
    if (!writer_->commit()) {
      throw illegal_state{"Failed to commit index metadata."};
    }
#ifndef __APPLE__
    // atomic_store may throw
    static_assert(!noexcept(
      std::atomic_store(&committed_state_, std::move(pending_state_.commit))));
#endif
    std::atomic_store(&committed_state_, std::move(pending_state_.commit));
  } catch (...) {
    abort();  // rollback transaction

    throw;
  }

  // after here transaction successfull (only noexcept operations below)

  // update 'last_gen_' to last commited/valid generation
  meta_.last_gen_ = committed_state_->first->gen_;
}

void index_writer::abort() {
  assert(!commit_lock_.try_lock());  // already locked

  if (!pending_state_) {
    // there is no open transaction
    return;
  }

  // all functions below are noexcept

  // guarded by commit_lock_
  writer_->rollback();
  pending_state_.reset();

  // reset actual meta, note that here we don't change
  // segment counters since it can be changed from insert function
  meta_.reset(*(committed_state_->first));
}

}  // namespace iresearch
