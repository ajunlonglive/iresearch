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

#include "shared.hpp"
#include "skip_list.hpp"

#include "store/store_utils.hpp"

#include "index/iterators.hpp"

#include "utils/math_utils.hpp"
#include "utils/std.hpp"

namespace {

// returns maximum number of skip levels needed to store specified
// count of objects for skip list with
// step skip_0 for 0 level, step skip_n for other levels
constexpr size_t max_levels(size_t skip_0, size_t skip_n,
                            size_t count) noexcept {
  return skip_0 < count ? 1 + irs::math::log(count / skip_0, skip_n) : 0;
}

}  // namespace

namespace iresearch {

void SkipWriter::Prepare(
  size_t max_levels, size_t count,
  const memory_allocator& alloc /* = memory_allocator::global() */) {
  max_levels_ =
    std::clamp(max_levels, size_t{1}, ::max_levels(skip_0_, skip_n_, count));
  levels_.reserve(max_levels_);

  // reset existing skip levels
  for (auto& level : levels_) {
    level.reset(alloc);
  }

  // add new skip levels if needed
  for (auto size = std::size(levels_); size < max_levels_; ++size) {
    levels_.emplace_back(alloc);
  }
}

void SkipWriter::Flush(index_output& out) {
  const auto rbegin =
    std::make_reverse_iterator(std::begin(levels_) + max_levels_);
  const auto rend = std::rend(levels_);

  // find first filled level
  auto level = std::find_if(rbegin, rend, [](const memory_output& level) {
    return level.stream.file_pointer();
  });

  // write number of levels
  const auto num_levels = static_cast<uint32_t>(std::distance(level, rend));
  out.write_vint(num_levels);

  // write levels from n downto 0
  for (; level != rend; ++level) {
    auto& stream = level->stream;
    stream.flush();  // update length of each buffer

    const uint64_t length = stream.file_pointer();
    assert(length);
    out.write_vlong(length);
    stream >> out;
  }
}

SkipReaderBase::Level::Level(index_input::ptr&& stream, doc_id_t step,
                             doc_id_t left, uint64_t begin) noexcept
  : stream{std::move(stream)},  // thread-safe input
    begin{begin},
    left{left},
    step{step} {}

void SkipReaderBase::Reset() {
  for (auto& level : levels_) {
    level.stream->seek(level.begin);
    if (level.child != kUndefined) {
      level.child = 0;
    }
    level.left = docs_count_;
  }
}

void SkipReaderBase::Prepare(index_input::ptr&& in, doc_id_t left) {
  assert(in);

  if (uint32_t max_levels = in->read_vint(); max_levels) {
    decltype(levels_) levels;
    levels.reserve(max_levels);

    auto load_level = [&levels, left](index_input::ptr stream, doc_id_t step) {
      assert(stream);

      // read level length
      const auto length = stream->read_vlong();

      if (!length) {
        throw index_error("while loading level, error: zero length");
      }

      const auto begin = stream->file_pointer();

      levels.emplace_back(std::move(stream), step, left, begin);

      return begin + length;
    };

    // skip step of the level
    size_t step =
      skip_0_ * static_cast<size_t>(std::pow(skip_n_, --max_levels));

    // load levels from n down to 1
    for (; max_levels; --max_levels) {
      const auto offset = load_level(in->dup(), step);

      // seek to the next level
      in->seek(offset);

      step /= skip_n_;
    }

    // load 0 level
    load_level(std::move(in), skip_0_);
    levels.back().child = kUndefined;

    levels_ = std::move(levels);
    docs_count_ = left;
  }
}

}  // namespace iresearch
