////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////

#include "columnstore2.hpp"

#include "error/error.hpp"
#include "formats/format_utils.hpp"
#include "index/file_names.hpp"
#include "search/all_iterator.hpp"
#include "search/score.hpp"
#include "utils/compression.hpp"
#include "utils/directory_utils.hpp"
#include "utils/string_utils.hpp"

namespace {

using namespace irs;
using namespace irs::columnstore2;

using column_ptr = std::unique_ptr<column_reader>;
using column_index = std::vector<sparse_bitmap_writer::block>;

constexpr SparseBitmapVersion ToSparseBitmapVersion(
  const column_info& info) noexcept {
  static_assert(SparseBitmapVersion::kPrevDoc == SparseBitmapVersion{1});

  return SparseBitmapVersion{info.track_prev_doc};
}

constexpr SparseBitmapVersion ToSparseBitmapVersion(
  ColumnProperty prop) noexcept {
  static_assert(SparseBitmapVersion::kPrevDoc == SparseBitmapVersion{1});

  return SparseBitmapVersion{ColumnProperty::kPrevDoc ==
                             (prop & ColumnProperty::kPrevDoc)};
}

std::string data_file_name(std::string_view prefix) {
  return file_name(prefix, writer::kDataFormatExt);
}

std::string index_file_name(std::string_view prefix) {
  return file_name(prefix, writer::kIndexFormatExt);
}

void write_header(index_output& out, const column_header& hdr) {
  out.write_long(hdr.docs_index);
  assert(hdr.id < std::numeric_limits<uint32_t>::max());
  out.write_int(static_cast<uint32_t>(hdr.id & 0xFFFFFFFF));
  out.write_int(hdr.min);
  out.write_int(hdr.docs_count);
  write_enum(out, hdr.type);
  write_enum(out, hdr.props);
}

column_header read_header(index_input& in) {
  column_header hdr;
  hdr.docs_index = in.read_long();
  hdr.id = in.read_int();
  hdr.min = in.read_int();
  hdr.docs_count = in.read_int();
  hdr.type = read_enum<ColumnType>(in);
  hdr.props = read_enum<ColumnProperty>(in);
  return hdr;
}

bool is_encrypted(const column_header& hdr) noexcept {
  return ColumnProperty::kEncrypt == (hdr.props & ColumnProperty::kEncrypt);
}

void write_bitmap_index(index_output& out,
                        std::span<const sparse_bitmap_writer::block> blocks) {
  const uint32_t count = static_cast<uint32_t>(blocks.size());

  if (count > 2) {
    out.write_int(count);
    for (auto& block : blocks) {
      out.write_int(block.index);
      out.write_int(block.offset);
    }
  } else {
    out.write_int(0);
  }
}

column_index read_bitmap_index(index_input& in) {
  const uint32_t count = in.read_int();

  if (count > std::numeric_limits<uint16_t>::max()) {
    throw index_error("Invalid number of blocks in column index");
  }

  if (count > 2) {
    column_index blocks(count);

    in.read_bytes(reinterpret_cast<byte_type*>(blocks.data()),
                  count * sizeof(sparse_bitmap_writer::block));

    if constexpr (!is_big_endian()) {
      for (auto& block : blocks) {
        block.index = numeric_utils::ntoh32(block.index);
        block.offset = numeric_utils::ntoh32(block.offset);
      }
    }

    return blocks;
  }

  return {};
}

void write_blocks_sparse(index_output& out,
                         std::span<const column::column_block> blocks) {
  // FIXME optimize
  for (auto& block : blocks) {
    out.write_long(block.addr);
    out.write_long(block.avg);
    out.write_byte(static_cast<byte_type>(block.bits));
    out.write_long(block.data);
    out.write_long(block.last_size);
  }
}

void write_blocks_dense(index_output& out,
                        std::span<const column::column_block> blocks) {
  // FIXME optimize
  for (auto& block : blocks) {
    out.write_long(block.data);
  }
}

std::vector<uint64_t> read_blocks_dense(const column_header& hdr,
                                        index_input& in) {
  std::vector<uint64_t> blocks(
    math::div_ceil32(hdr.docs_count, column::kBlockSize));

  in.read_bytes(reinterpret_cast<byte_type*>(blocks.data()),
                sizeof(uint64_t) * blocks.size());

  if constexpr (!is_big_endian()) {
    // FIXME simd?
    for (auto& block : blocks) {
      block = numeric_utils::ntoh64(block);
    }
  }

  return blocks;
}

// Iterator over a specified contiguous range of documents
template<typename PayloadReader>
class range_column_iterator final : public resettable_doc_iterator,
                                    private PayloadReader {
 private:
  using payload_reader = PayloadReader;

  // FIXME(gnusi):
  //  * don't expose payload for noop_value_reader?
  //  * don't expose prev_doc if not requested?
  using attributes = std::tuple<document, cost, score, prev_doc, irs::payload>;

 public:
  template<typename... Args>
  range_column_iterator(const column_header& header, bool track_prev,
                        Args&&... args)
    : payload_reader{std::forward<Args>(args)...},
      min_base_{header.min},
      min_doc_{min_base_},
      max_doc_{min_base_ + header.docs_count - 1} {
    assert(min_doc_ <= max_doc_);
    assert(!doc_limits::eof(max_doc_));
    std::get<cost>(attrs_).reset(header.docs_count);
    if (track_prev) {
      std::get<prev_doc>(attrs_).reset(
        [](const void* ctx) noexcept {
          auto* self = static_cast<const range_column_iterator*>(ctx);
          const auto value = self->value();
          const auto max_doc = self->max_doc_;

          if (IRS_LIKELY(self->min_base_ < value && value <= max_doc)) {
            return value - 1;
          }

          if (value > max_doc) {
            return max_doc;
          }

          return doc_limits::invalid();
        },
        this);
    }
  }

  virtual attribute* get_mutable(
    irs::type_info::type_id type) noexcept override {
    return irs::get_mutable(attrs_, type);
  }

  virtual doc_id_t value() const noexcept override {
    return std::get<document>(attrs_).value;
  }

  virtual doc_id_t seek(irs::doc_id_t doc) override {
    if (IRS_LIKELY(min_doc_ <= doc && doc <= max_doc_)) {
      std::get<document>(attrs_).value = doc;
      min_doc_ = doc + 1;
      std::get<irs::payload>(attrs_).value = this->payload(doc - min_base_);
      return doc;
    }

    if (!doc_limits::valid(value())) {
      std::get<document>(attrs_).value = min_doc_++;
      std::get<irs::payload>(attrs_).value = this->payload(value() - min_base_);
      return value();
    }

    if (value() < doc) {
      max_doc_ = doc_limits::invalid();
      min_doc_ = doc_limits::eof();
      std::get<document>(attrs_).value = doc_limits::eof();
      std::get<irs::payload>(attrs_).value = {};
      return doc_limits::eof();
    }

    return value();
  }

  virtual bool next() override {
    if (min_doc_ <= max_doc_) {
      std::get<document>(attrs_).value = min_doc_++;
      std::get<irs::payload>(attrs_).value = this->payload(value() - min_base_);
      return true;
    }

    std::get<document>(attrs_).value = doc_limits::eof();
    std::get<irs::payload>(attrs_).value = {};
    return false;
  }

  virtual void reset() noexcept override {
    min_doc_ = min_base_;
    max_doc_ = min_doc_ + std::get<cost>(attrs_).estimate() - 1;
    std::get<document>(attrs_).value = doc_limits::invalid();
  }

 private:
  doc_id_t min_base_;
  doc_id_t min_doc_;
  doc_id_t max_doc_;
  attributes attrs_;
};

// Iterator over a specified bitmap of documents
template<typename PayloadReader>
class bitmap_column_iterator final : public resettable_doc_iterator,
                                     private PayloadReader {
 private:
  using payload_reader = PayloadReader;

  using attributes =
    std::tuple<attribute_ptr<document>, cost, attribute_ptr<score>,
               attribute_ptr<prev_doc>, irs::payload>;

 public:
  template<typename... Args>
  bitmap_column_iterator(index_input::ptr&& bitmap_in,
                         const sparse_bitmap_iterator::options& opts,
                         cost::cost_t cost, Args&&... args)
    : payload_reader{std::forward<Args>(args)...},
      bitmap_{std::move(bitmap_in), opts, cost} {
    std::get<irs::cost>(attrs_).reset(cost);
    std::get<attribute_ptr<document>>(attrs_) =
      irs::get_mutable<document>(&bitmap_);
    std::get<attribute_ptr<score>>(attrs_) = irs::get_mutable<score>(&bitmap_);
    std::get<attribute_ptr<prev_doc>>(attrs_) =
      irs::get_mutable<prev_doc>(&bitmap_);
  }

  virtual attribute* get_mutable(
    irs::type_info::type_id type) noexcept override {
    return irs::get_mutable(attrs_, type);
  }

  virtual doc_id_t value() const noexcept override {
    return std::get<attribute_ptr<document>>(attrs_).ptr->value;
  }

  virtual doc_id_t seek(irs::doc_id_t doc) override {
    assert(doc_limits::valid(doc) || doc_limits::valid(value()));
    doc = bitmap_.seek(doc);

    if (!doc_limits::eof(doc)) {
      std::get<irs::payload>(attrs_).value = this->payload(bitmap_.index());
      return doc;
    }

    std::get<irs::payload>(attrs_).value = {};
    return doc_limits::eof();
  }

  virtual bool next() override {
    if (bitmap_.next()) {
      std::get<irs::payload>(attrs_).value = this->payload(bitmap_.index());
      return true;
    }

    std::get<irs::payload>(attrs_).value = {};
    return false;
  }

  virtual void reset() override { bitmap_.reset(); }

 private:
  sparse_bitmap_iterator bitmap_;
  attributes attrs_;
};

class column_base : public column_reader, private util::noncopyable {
 public:
  column_base(std::optional<std::string>&& name, bstring&& payload,
              column_header&& hdr, column_index&& index,
              const index_input& stream, encryption::stream* cipher)
    : stream_{&stream},
      cipher_{cipher},
      hdr_{std::move(hdr)},
      index_{std::move(index)},
      payload_{std::move(payload)},
      name_{std::move(name)} {
    assert(!is_encrypted(hdr_) || cipher_);
  }

  ~column_base() override {
    if (!column_data_.empty()) {
      auto released = static_cast<int64_t>(column_data_.size());
      // force memory release
      column_data_ = {};
      if (buffered_memory_accounter_) {
        buffered_memory_accounter_(-released);
      }
    }
  }

  std::string_view name() const final {
    return name_.has_value() ? name_.value() : std::string_view{};
  }

  virtual field_id id() const noexcept final { return hdr_.id; }

  virtual bytes_view payload() const final { return payload_; }

  virtual doc_id_t size() const noexcept final { return hdr_.docs_count; }

  const column_header& header() const noexcept { return hdr_; }

  bool track_prev_doc(ColumnHint hint) const noexcept {
    return ColumnHint::kPrevDoc == (hint & ColumnHint::kPrevDoc);
  }

  sparse_bitmap_iterator::options bitmap_iterator_options(
    ColumnHint hint) const noexcept {
    return {.version = ToSparseBitmapVersion(header().props),
            .track_prev_doc = track_prev_doc(hint),
            .use_block_index = true,
            .blocks = index_};
  }

  const index_input& stream() const noexcept {
    assert(stream_);
    return *stream_;
  }

  virtual void make_buffered(irs::index_input&, memory_accounting_f&,
                             std::span<std::unique_ptr<column_reader>>) {}

 protected:
  template<typename Factory>
  doc_iterator::ptr make_iterator(Factory&& f, ColumnHint hint) const;

  column_header& mutable_header() { return hdr_; }
  void reset_stream(const index_input* stream) { stream_ = stream; }

  bool allocate_buffered_memory(
    size_t size, memory_accounting_f& buffered_memory_accounter) {
    assert(size < static_cast<size_t>(std::numeric_limits<int64_t>::max()));
    assert(buffered_memory_accounter);
    if (!buffered_memory_accounter(static_cast<int64_t>(size))) {
      auto column_name = name();
      if (irs::IsNull(column_name)) {
        column_name = "<anonymous>";
      }
      // use string to have always null-terminated name
      std::string col{column_name};
      IR_FRMT_WARN(
        "Failed to allocate memory for buffered column id %llu name: %s of "
        "size " IR_SIZE_T_SPECIFIER,
        header().id, col.c_str(), size);
      return false;
    }
    // should be only one alllocation
    assert(column_data_.empty());
    column_data_.resize(size);
    buffered_memory_accounter_ = buffered_memory_accounter;
    return true;
  }

  size_t calculate_bitmap_size(size_t file_len,
                               std::span<std::unique_ptr<column_reader>>
                                 next_sorted_columns) const noexcept {
    if (!header().docs_index) {
      return 0;
    }
    for (const auto& c : next_sorted_columns) {
      auto column = static_cast<column_base*>(c.get());
      assert(column != nullptr);
      if (column->header().docs_index) {
        file_len = column->header().docs_index;
        break;
      }
    }
    assert(header().docs_index < file_len);
    return file_len - header().docs_index;
  }

  void store_bitmap_index(size_t bitmap_size, size_t buffer_offset,
                          remapped_bytes_view_input::mapping* mapping,
                          column_header& hdr, index_input& in) {
    assert(bitmap_size);
    assert(hdr.docs_index);
    in.read_bytes(hdr.docs_index, column_data_.data() + buffer_offset,
                  bitmap_size);
    assert(mapping || !is_encrypted(hdr));
    if (is_encrypted(hdr) && mapping) {
      mapping->emplace_back(hdr.docs_index, buffer_offset);
    } else {
      hdr.docs_index = buffer_offset;
    }
  }

  std::vector<irs::byte_type> column_data_;
  irs::index_input::ptr buffered_input_;

 private:
  template<typename ValueReader>
  doc_iterator::ptr make_iterator(ValueReader&& f, index_input::ptr&& in,
                                  ColumnHint hint) const;

  memory_accounting_f buffered_memory_accounter_;
  const index_input* stream_;
  encryption::stream* cipher_;
  column_header hdr_;
  column_index index_;
  bstring payload_;
  std::optional<std::string> name_;
};

template<typename ValueReader>
doc_iterator::ptr column_base::make_iterator(ValueReader&& rdr,
                                             index_input::ptr&& index_in,
                                             ColumnHint hint) const {
  if (!index_in) {
    using iterator_type = range_column_iterator<ValueReader>;

    return memory::make_managed<iterator_type>(header(), track_prev_doc(hint),
                                               std::move(rdr));
  } else {
    index_in->seek(header().docs_index);

    using iterator_type = bitmap_column_iterator<ValueReader>;

    return memory::make_managed<iterator_type>(
      std::move(index_in), bitmap_iterator_options(hint), header().docs_count,
      std::move(rdr));
  }
}

template<typename Factory>
doc_iterator::ptr column_base::make_iterator(Factory&& f,
                                             ColumnHint hint) const {
  assert(header().docs_count);

  index_input::ptr value_in = stream().reopen();

  if (!value_in) {
    // implementation returned wrong pointer
    IR_FRMT_ERROR("Failed to reopen input in: %s", __FUNCTION__);

    throw io_error{"failed to reopen input"};
  }

  index_input::ptr index_in = nullptr;

  if (0 != header().docs_index) {
    index_in = value_in->dup();

    if (!index_in) {
      // implementation returned wrong pointer
      IR_FRMT_ERROR("Failed to duplicate input in: %s", __FUNCTION__);

      throw io_error{"failed to duplicate input"};
    }
  }

  if (is_encrypted(header())) {
    assert(cipher_);
    return make_iterator(f(std::move(value_in), *cipher_), std::move(index_in),
                         hint);
  } else {
    const byte_type* data =
      value_in->read_buffer(0, value_in->length(), BufferHint::PERSISTENT);

    if (data) {
      // direct buffer access
      return make_iterator(f(data), std::move(index_in), hint);
    }

    return make_iterator(f(std::move(value_in)), std::move(index_in), hint);
  }
}

struct noop_value_reader {
  constexpr bytes_view payload(doc_id_t) const noexcept { return {}; }
};

class value_direct_reader {
 protected:
  explicit value_direct_reader(const byte_type* data) noexcept : data_{data} {
    assert(data);
  }

  bytes_view value(uint64_t offset, size_t length) noexcept {
    return {data_ + offset, length};
  }

  const byte_type* data_;
};

template<bool Resize>
class value_reader {
 protected:
  value_reader(index_input::ptr data_in, size_t size)
    : buf_(size, 0), data_in_{std::move(data_in)} {}

  bytes_view value(uint64_t offset, size_t length) {
    if constexpr (Resize) {
      buf_.resize(length);
    }

    auto* buf = buf_.data();

    [[maybe_unused]] const size_t read =
      data_in_->read_bytes(offset, buf, length);
    assert(read == length);

    return {buf, length};
  }

  bstring buf_;
  index_input::ptr data_in_;
};

template<bool Resize>
class encrypted_value_reader {
 protected:
  encrypted_value_reader(index_input::ptr&& data_in, encryption::stream* cipher,
                         size_t size)
    : buf_(size, 0), data_in_{std::move(data_in)}, cipher_{cipher} {}

  bytes_view value(uint64_t offset, size_t length) {
    if constexpr (Resize) {
      buf_.resize(length);
    }

    auto* buf = buf_.data();

    [[maybe_unused]] const size_t read =
      data_in_->read_bytes(offset, buf, length);
    assert(read == length);

    [[maybe_unused]] const bool ok = cipher_->decrypt(offset, buf, length);
    assert(ok);

    return {buf, length};
  }

  bstring buf_;
  index_input::ptr data_in_;
  encryption::stream* cipher_;
};

doc_iterator::ptr make_mask_iterator(const column_base& column,
                                     ColumnHint hint) {
  const auto& header = column.header();

  if (0 == header.docs_count) {
    // only mask column can be empty
    return doc_iterator::empty();
  }

  if (0 == header.docs_index) {
    return memory::make_managed<range_column_iterator<noop_value_reader>>(
      header, column.track_prev_doc(hint));
  }

  auto dup = column.stream().reopen();

  if (!dup) {
    // implementation returned wrong pointer
    IR_FRMT_ERROR("Failed to reopen input in: %s", __FUNCTION__);

    throw io_error{"failed to reopen input"};
  }

  dup->seek(header.docs_index);

  return memory::make_managed<sparse_bitmap_iterator>(
    std::move(dup), column.bitmap_iterator_options(hint), header.docs_count);
}

struct mask_column final : public column_base {
  static column_ptr read(std::optional<std::string>&& name, bstring&& payload,
                         column_header&& hdr, column_index&& index,
                         index_input& /*index_in*/, const index_input& data_in,
                         compression::decompressor::ptr&& /*inflater*/,
                         encryption::stream* cipher) {
    return std::make_unique<mask_column>(std::move(name), std::move(payload),
                                         std::move(hdr), std::move(index),
                                         data_in, cipher);
  }

  mask_column(std::optional<std::string>&& name, bstring&& payload,
              column_header&& hdr, column_index&& index,
              const index_input& data_in, encryption::stream* cipher)
    : column_base{std::move(name), std::move(payload),
                  std::move(hdr),  std::move(index),
                  data_in,         cipher} {
    assert(ColumnType::kMask == header().type);
  }

  virtual doc_iterator::ptr iterator(ColumnHint hint) const override;
};

doc_iterator::ptr mask_column::iterator(ColumnHint hint) const {
  return make_mask_iterator(*this, hint);
}

class dense_fixed_length_column final : public column_base {
 public:
  static column_ptr read(std::optional<std::string>&& name, bstring&& payload,
                         column_header&& hdr, column_index&& index,
                         index_input& index_in, const index_input& data_in,
                         compression::decompressor::ptr&& inflater,
                         encryption::stream* cipher);

  dense_fixed_length_column(std::optional<std::string>&& name,
                            bstring&& payload, column_header&& hdr,
                            column_index&& index, const index_input& data_in,
                            compression::decompressor::ptr&& inflater,
                            encryption::stream* cipher, uint64_t data,
                            uint64_t len)
    : column_base{std::move(name), std::move(payload),
                  std::move(hdr),  std::move(index),
                  data_in,         cipher},
      inflater_{std::move(inflater)},
      data_{data},
      len_{len} {
    assert(header().docs_count);
    assert(ColumnType::kDenseFixed == header().type);
  }

  virtual doc_iterator::ptr iterator(ColumnHint hint) const override;

  void make_buffered(
    irs::index_input& in, memory_accounting_f& memory_accounter,
    std::span<std::unique_ptr<column_reader>> next_sorted_columns) final {
    auto& hdr = mutable_header();
    auto const data_size = len_ * hdr.docs_count;
    auto const bitmap_size =
      calculate_bitmap_size(in.length(), next_sorted_columns);
    auto const total_size = data_size + bitmap_size;
    if (!allocate_buffered_memory(total_size, memory_accounter)) {
      return;
    }
    in.read_bytes(data_, column_data_.data(), data_size);
    irs::remapped_bytes_view_input::mapping mapping;
    if (bitmap_size) {
      store_bitmap_index(bitmap_size, data_size, &mapping, hdr, in);
    }
    if (is_encrypted(hdr)) {
      mapping.emplace_back(data_, 0);
      buffered_input_ = std::make_unique<irs::remapped_bytes_view_input>(
        bytes_view{column_data_.data(), column_data_.size()},
        std::move(mapping));
    } else {
      buffered_input_ = std::make_unique<bytes_view_input>(
        bytes_view{column_data_.data(), column_data_.size()});
      data_ = 0;
    }
    reset_stream(buffered_input_.get());
  }

 private:
  template<typename ValueReader>
  class payload_reader : private ValueReader {
   public:
    template<typename... Args>
    payload_reader(uint64_t data, uint64_t len, Args&&... args)
      : ValueReader{std::forward<Args>(args)...}, data_{data}, len_{len} {}

    bytes_view payload(doc_id_t i) {
      const auto offset = data_ + len_ * i;

      return ValueReader::value(offset, len_);
    }

   private:
    uint64_t data_;  // where data starts
    uint64_t len_;   // data entry length
  };

  compression::decompressor::ptr inflater_;
  uint64_t data_;
  uint64_t len_;
};

/*static*/ column_ptr dense_fixed_length_column::read(
  std::optional<std::string>&& name, bstring&& payload, column_header&& hdr,
  column_index&& index, index_input& index_in, const index_input& data_in,
  compression::decompressor::ptr&& inflater, encryption::stream* cipher) {
  const uint64_t len = index_in.read_long();
  const uint64_t data = index_in.read_long();

  return std::make_unique<dense_fixed_length_column>(
    std::move(name), std::move(payload), std::move(hdr), std::move(index),
    data_in, std::move(inflater), cipher, data, len);
}

doc_iterator::ptr dense_fixed_length_column::iterator(ColumnHint hint) const {
  if (ColumnHint::kMask == (ColumnHint::kMask & hint)) {
    return make_mask_iterator(*this, hint);
  }

  struct factory {
    payload_reader<encrypted_value_reader<false>> operator()(
      index_input::ptr&& stream, encryption::stream& cipher) const {
      return {ctx->data_, ctx->len_, std::move(stream), &cipher, ctx->data_};
    };

    payload_reader<value_reader<false>> operator()(
      index_input::ptr&& stream) const {
      return {ctx->data_, ctx->len_, std::move(stream), ctx->data_};
    }

    payload_reader<value_direct_reader> operator()(
      const byte_type* data) const {
      return {ctx->data_, ctx->len_, data};
    }

    const dense_fixed_length_column* ctx;
  };

  return make_iterator(factory{this}, hint);
}

class fixed_length_column final : public column_base {
 public:
  static column_ptr read(std::optional<std::string>&& name, bstring&& payload,
                         column_header&& hdr, column_index&& index,
                         index_input& index_in, const index_input& data_in,
                         compression::decompressor::ptr&& inflater,
                         encryption::stream* cipher) {
    const uint64_t len = index_in.read_long();
    auto blocks = read_blocks_dense(hdr, index_in);

    return std::make_unique<fixed_length_column>(
      std::move(name), std::move(payload), std::move(hdr), std::move(index),
      data_in, std::move(inflater), cipher, std::move(blocks), len);
  }

  fixed_length_column(std::optional<std::string>&& name, bstring&& payload,
                      column_header&& hdr, column_index&& index,
                      const index_input& data_in,
                      compression::decompressor::ptr&& inflater,
                      encryption::stream* cipher,
                      std::vector<uint64_t>&& blocks, uint64_t len)
    : column_base{std::move(name), std::move(payload),
                  std::move(hdr),  std::move(index),
                  data_in,         cipher},
      blocks_{blocks},
      inflater_{std::move(inflater)},
      len_{len} {
    assert(header().docs_count);
    assert(ColumnType::kFixed == header().type);
  }

  virtual doc_iterator::ptr iterator(ColumnHint hint) const override;

  void make_buffered(
    irs::index_input& in, memory_accounting_f& memory_accounter,
    std::span<std::unique_ptr<column_reader>> next_sorted_columns) override {
    auto& hdr = mutable_header();
    if (!is_encrypted(hdr)) {
      if (make_buffered_data<false>(len_, hdr, in, blocks_, column_data_,
                                    next_sorted_columns, memory_accounter,
                                    nullptr)) {
        buffered_input_ = std::make_unique<bytes_view_input>(
          bytes_view{column_data_.data(), column_data_.size()});
      }
    } else {
      irs::remapped_bytes_view_input::mapping mapping;
      if (make_buffered_data<true>(len_, hdr, in, blocks_, column_data_,
                                   next_sorted_columns, memory_accounter,
                                   &mapping)) {
        buffered_input_ = std::make_unique<irs::remapped_bytes_view_input>(
          bytes_view{column_data_.data(), column_data_.size()},
          std::move(mapping));
      }
    }
    if (buffered_input_) {
      reset_stream(buffered_input_.get());
    }
  }

 private:
  using column_block = uint64_t;

  template<typename ValueReader>
  class payload_reader : private ValueReader {
   public:
    template<typename... Args>
    payload_reader(const column_block* blocks, uint64_t len, Args&&... args)
      : ValueReader{std::forward<Args>(args)...}, blocks_{blocks}, len_{len} {}

    bytes_view payload(doc_id_t i) {
      const auto block_idx = i / column::kBlockSize;
      const auto value_idx = i % column::kBlockSize;

      const auto offset = blocks_[block_idx] + len_ * value_idx;

      return ValueReader::value(offset, len_);
    }

   private:
    const column_block* blocks_;
    uint64_t len_;
  };  // payload_reader

  template<bool encrypted>
  bool make_buffered_data(
    uint64_t len, column_header& hdr, irs::index_input& in,
    std::vector<uint64_t>& blocks, std::vector<irs::byte_type>& column_data,
    std::span<std::unique_ptr<column_reader>> next_sorted_columns,
    memory_accounting_f& memory_accounter,
    remapped_bytes_view_input::mapping* mapping) {
    assert(!blocks.empty());
    auto const last_block_full = hdr.docs_count % column::kBlockSize == 0;
    auto last_offset = blocks.back();
    std::vector<std::tuple<size_t, size_t, size_t>> blocks_offsets;
    size_t blocks_data_size{0};
    size_t block_index{0};
    blocks_offsets.reserve(blocks.size());
    for (auto& block : blocks) {
      size_t length = (block != last_offset || last_block_full)
                        ? column::kBlockSize
                        : hdr.docs_count % column::kBlockSize;
      length = length * len;
      blocks_offsets.emplace_back(block_index++, blocks_data_size, length);
      blocks_data_size += length;
    }
    const auto bitmap_index_size =
      calculate_bitmap_size(in.length(), next_sorted_columns);
    if (!allocate_buffered_memory(bitmap_index_size + blocks_data_size,
                                  memory_accounter)) {
      return false;
    }
    std::sort(blocks_offsets.begin(), blocks_offsets.end(),
              [&blocks](const auto& lhs, const auto& rhs) {
                return blocks[std::get<0>(lhs)] < blocks[std::get<0>(rhs)];
              });
    for (const auto& offset : blocks_offsets) {
      auto& block = blocks[std::get<0>(offset)];
      in.read_bytes(block, column_data.data() + std::get<1>(offset),
                    std::get<2>(offset));
      if constexpr (encrypted) {
        assert(mapping);
        mapping->emplace_back(block, std::get<1>(offset));
      } else {
        block = std::get<1>(offset);
      }
    }
    if (bitmap_index_size) {
      store_bitmap_index(bitmap_index_size, blocks_data_size, mapping, hdr, in);
    }
    return true;
  }

  std::vector<uint64_t> blocks_;
  compression::decompressor::ptr inflater_;
  uint64_t len_;
};

doc_iterator::ptr fixed_length_column::iterator(ColumnHint hint) const {
  if (ColumnHint::kMask == (ColumnHint::kMask & hint)) {
    return make_mask_iterator(*this, hint);
  }

  struct factory {
    payload_reader<encrypted_value_reader<false>> operator()(
      index_input::ptr&& stream, encryption::stream& cipher) const {
      return {ctx->blocks_.data(), ctx->len_, std::move(stream), &cipher,
              ctx->len_};
    };

    payload_reader<value_reader<false>> operator()(
      index_input::ptr&& stream) const {
      return {ctx->blocks_.data(), ctx->len_, std::move(stream), ctx->len_};
    }

    payload_reader<value_direct_reader> operator()(
      const byte_type* data) const {
      return {ctx->blocks_.data(), ctx->len_, data};
    }

    const fixed_length_column* ctx;
  };

  return make_iterator(factory{this}, hint);
}

class sparse_column final : public column_base {
 public:
  struct column_block : column::column_block {
    doc_id_t last;
  };

  static column_ptr read(std::optional<std::string>&& name, bstring&& payload,
                         column_header&& hdr, column_index&& index,
                         index_input& index_in, const index_input& data_in,
                         compression::decompressor::ptr&& inflater,
                         encryption::stream* cipher) {
    auto blocks = read_blocks_sparse(hdr, index_in);

    return std::make_unique<sparse_column>(
      std::move(name), std::move(payload), std::move(hdr), std::move(index),
      data_in, std::move(inflater), cipher, std::move(blocks));
  }

  sparse_column(std::optional<std::string>&& name, bstring&& payload,
                column_header&& hdr, column_index&& index,
                const index_input& data_in,
                compression::decompressor::ptr&& inflater,
                encryption::stream* cipher, std::vector<column_block>&& blocks)
    : column_base{std::move(name), std::move(payload),
                  std::move(hdr),  std::move(index),
                  data_in,         cipher},
      blocks_{std::move(blocks)},
      inflater_{std::move(inflater)} {
    assert(header().docs_count);
    assert(ColumnType::kSparse == header().type);
  }

  doc_iterator::ptr iterator(ColumnHint hint) const override;

  void make_buffered(
    irs::index_input& in, memory_accounting_f& memory_accounter,
    std::span<std::unique_ptr<column_reader>> next_sorted_columns) override {
    auto& hdr = mutable_header();
    if (!is_encrypted(hdr)) {
      if (make_buffered_data<false>(hdr, in, blocks_, column_data_,
                                    next_sorted_columns, memory_accounter,
                                    nullptr)) {
        buffered_input_ = std::make_unique<bytes_view_input>(
          bytes_view{column_data_.data(), column_data_.size()});
      }
    } else {
      irs::remapped_bytes_view_input::mapping mapping;
      if (make_buffered_data<true>(hdr, in, blocks_, column_data_,
                                   next_sorted_columns, memory_accounter,
                                   &mapping)) {
        buffered_input_ = std::make_unique<irs::remapped_bytes_view_input>(
          bytes_view{column_data_.data(), column_data_.size()},
          std::move(mapping));
      }
    }
    if (buffered_input_) {
      reset_stream(buffered_input_.get());
    }
  }

 private:
  static std::vector<column_block> read_blocks_sparse(const column_header& hdr,
                                                      index_input& in);

  template<typename ValueReader>
  class payload_reader : private ValueReader {
   public:
    template<typename... Args>
    payload_reader(const column_block* blocks, Args&&... args)
      : ValueReader{std::forward<Args>(args)...}, blocks_{blocks} {}

    bytes_view payload(doc_id_t i);

   private:
    const column_block* blocks_;
  };

  template<bool encrypted>
  bool make_buffered_data(
    column_header& hdr, irs::index_input& in, std::vector<column_block>& blocks,
    std::vector<irs::byte_type>& column_data,
    std::span<std::unique_ptr<column_reader>> next_sorted_columns,
    memory_accounting_f& memory_accounter,
    remapped_bytes_view_input::mapping* mapping) {
    // idx adr/block offset length source
    std::vector<std::tuple<size_t, bool, size_t, size_t, size_t>> chunks;
    size_t chunks_size{0};
    size_t block_idx{0};
    std::vector<irs::byte_type> addr_buffer;
    chunks.reserve(blocks.size());  // minimum, we may even need more chunks
    for (auto& block : blocks) {
      size_t length{0};
      if (bitpack::ALL_EQUAL == block.bits) {
        length = block.avg * block.last;
        length += block.last_size;
      } else {
        // addr table size
        const size_t addr_length = packed::bytes_required_64(
          math::ceil64((block.last + 1), packed::BLOCK_SIZE_64), block.bits);
        chunks.emplace_back(block_idx, true, chunks_size, addr_length,
                            block.addr);
        chunks_size += addr_length;
        // block data size is calculated as end of the data for the last
        // document in block
        const size_t block_size = block.bits * sizeof(uint64_t);
        auto const value_index = block.last % packed::BLOCK_SIZE_64;
        addr_buffer.resize(block_size);
        in.read_bytes(block.addr + addr_length - block_size, addr_buffer.data(),
                      block_size);
        const uint64_t start_delta = zig_zag_decode64(packed::fastpack_at(
          reinterpret_cast<const uint64_t*>(addr_buffer.data()), value_index,
          block.bits));
        const uint64_t start = block.avg * block.last + start_delta;

        length = block.last_size + start;
      }
      // ALL_EQUAL could also be an empty block but still need this chunk to
      // properly calculate offsets
      assert(length || bitpack::ALL_EQUAL == block.bits);
      chunks.emplace_back(block_idx++, false, chunks_size, length, block.data);
      chunks_size += length;
    }
    const auto bitmap_size =
      calculate_bitmap_size(in.length(), next_sorted_columns);
    if (!allocate_buffered_memory(bitmap_size + chunks_size,
                                  memory_accounter)) {
      return false;
    }

    std::sort(chunks.begin(), chunks.end(),
              [](const auto& lhs, const auto& rhs) {
                return std::get<4>(lhs) < std::get<4>(rhs);
              });

    for (const auto& chunk : chunks) {
      auto& block = blocks[std::get<0>(chunk)];
      if (bitpack::ALL_EQUAL == block.bits || !std::get<1>(chunk)) {
        assert(block.data == std::get<4>(chunk));
        in.read_bytes(block.data, column_data.data() + std::get<2>(chunk),
                      std::get<3>(chunk));
        if constexpr (encrypted) {
          assert(mapping);
          mapping->emplace_back(block.data, std::get<2>(chunk));
        } else {
          block.data = std::get<2>(chunk);
          if (bitpack::ALL_EQUAL == block.bits) {
            block.addr = std::get<2>(chunk);
          }
        }
      } else {
        assert(std::get<1>(chunk));
        assert(block.addr == std::get<4>(chunk));
        in.read_bytes(block.addr, column_data.data() + std::get<2>(chunk),
                      std::get<3>(chunk));
        if constexpr (encrypted) {
          assert(mapping);
          mapping->emplace_back(block.addr, std::get<2>(chunk));
        } else {
          block.addr = std::get<2>(chunk);
        }
      }
    }
    if (bitmap_size) {
      store_bitmap_index(bitmap_size, chunks_size, mapping, hdr, in);
    }
    return true;
  }

  std::vector<column_block> blocks_;
  compression::decompressor::ptr inflater_;
};

template<typename ValueReader>
bytes_view sparse_column::payload_reader<ValueReader>::payload(doc_id_t i) {
  const auto& block = blocks_[i / column::kBlockSize];
  const size_t index = i % column::kBlockSize;

  if (bitpack::ALL_EQUAL == block.bits) {
    const size_t addr = block.data + block.avg * index;

    size_t length = block.avg;
    if (IRS_UNLIKELY(block.last == index)) {
      length = block.last_size;
    }

    return ValueReader::value(addr, length);
  }

  const size_t block_size = block.bits * sizeof(uint64_t);
  const size_t block_index = index / packed::BLOCK_SIZE_64;
  size_t value_index = index % packed::BLOCK_SIZE_64;

  const size_t addr_offset = block.addr + block_index * block_size;

  const byte_type* addr_buf;
  if constexpr (std::is_same_v<ValueReader, value_direct_reader>) {
    addr_buf = this->data_ + addr_offset;
  } else {
    this->buf_.resize(block_size);
    this->data_in_->read_bytes(addr_offset, this->buf_.data(), block_size);
    addr_buf = this->buf_.c_str();
  }
  const uint64_t start_delta = zig_zag_decode64(packed::fastpack_at(
    reinterpret_cast<const uint64_t*>(addr_buf), value_index, block.bits));
  const uint64_t start = block.avg * index + start_delta;

  size_t length = block.last_size;
  if (IRS_LIKELY(block.last != index)) {
    if (IRS_UNLIKELY(++value_index == 64)) {
      value_index = 0;

      if constexpr (std::is_same_v<ValueReader, value_direct_reader>) {
        addr_buf += block_size;
      } else {
        this->buf_.resize(block_size);
        this->data_in_->read_bytes(this->buf_.data(), block_size);
        addr_buf = this->buf_.c_str();
      }
    }

    const uint64_t end_delta = zig_zag_decode64(packed::fastpack_at(
      reinterpret_cast<const uint64_t*>(addr_buf), value_index, block.bits));
    length = end_delta - start_delta + block.avg;
  }

  const auto offset = block.data + start;

  return ValueReader::value(offset, length);
}

/*static*/ std::vector<sparse_column::column_block>
sparse_column::read_blocks_sparse(const column_header& hdr, index_input& in) {
  std::vector<sparse_column::column_block> blocks{
    math::div_ceil32(hdr.docs_count, column::kBlockSize)};

  // FIXME optimize
  for (auto& block : blocks) {
    block.addr = in.read_long();
    block.avg = in.read_long();
    block.bits = in.read_byte();
    block.data = in.read_long();
    block.last_size = in.read_long();
    block.last = column::kBlockSize - 1;
  }
  blocks.back().last = uint16_t(hdr.docs_count % column::kBlockSize - 1U);

  return blocks;
}

doc_iterator::ptr sparse_column::iterator(ColumnHint hint) const {
  if (ColumnHint::kMask == (ColumnHint::kMask & hint)) {
    return make_mask_iterator(*this, hint);
  }

  struct factory {
    payload_reader<encrypted_value_reader<true>> operator()(
      index_input::ptr&& stream, encryption::stream& cipher) const {
      return {ctx->blocks_.data(), std::move(stream), &cipher, size_t{0}};
    };

    payload_reader<value_reader<true>> operator()(
      index_input::ptr&& stream) const {
      return {ctx->blocks_.data(), std::move(stream), size_t{0}};
    }

    payload_reader<value_direct_reader> operator()(
      const byte_type* data) const {
      return {ctx->blocks_.data(), data};
    }

    const sparse_column* ctx;
  };

  return make_iterator(factory{this}, hint);
}

using column_factory_f = column_ptr (*)(std::optional<std::string>&&, bstring&&,
                                        column_header&&, column_index&&,
                                        index_input&, const index_input&,
                                        compression::decompressor::ptr&&,
                                        encryption::stream*);

constexpr column_factory_f kFactories[]{
  &sparse_column::read, &mask_column::read, &fixed_length_column::read,
  &dense_fixed_length_column::read};

bool less(std::string_view lhs, std::string_view rhs) noexcept {
  if (IsNull(lhs)) {
    return !IsNull(rhs);
  }

  if (IsNull(rhs)) {
    return false;
  }

  return lhs < rhs;
}

}  // namespace

namespace iresearch {
namespace columnstore2 {

void column::prepare(doc_id_t key) {
#ifdef IRESEARCH_DEBUG
  assert(!sealed_);
#endif

  if (IRS_LIKELY(key > pend_)) {
    if (addr_table_.full()) {
      flush_block();
    }

    prev_ = pend_;
    pend_ = key;
    docs_writer_.push_back(key);
    addr_table_.push_back(data_.stream.file_pointer());
  }
}

void column::reset() {
  if (addr_table_.empty()) {
    return;
  }

  [[maybe_unused]] const bool res = docs_writer_.erase(pend_);
  assert(res);
  data_.stream.seek(addr_table_.back());
  addr_table_.pop_back();
  pend_ = prev_;
}

void column::flush_block() {
  assert(!addr_table_.empty());
  assert(ctx_.data_out);
  data_.stream.flush();

  auto& data_out = *ctx_.data_out;
  auto& block = blocks_.emplace_back();

  block.addr = data_out.file_pointer();
  block.last_size = data_.file.length() - addr_table_.back();

  const uint32_t docs_count = addr_table_.size();
  const uint64_t addr_table_size =
    math::ceil64(docs_count, packed::BLOCK_SIZE_64);
  auto* begin = addr_table_.begin();
  auto* end = begin + addr_table_size;

  bool all_equal = !data_.file.length();
  if (!all_equal) {
    std::tie(block.data, block.avg, all_equal) =
      encode::avg::encode(begin, addr_table_.current());
  } else {
    block.avg = 0;
    block.data = data_out.file_pointer();
  }

  if (all_equal) {
    assert(simd::all_equal<false>(begin, addr_table_size) && (0 == *begin));
    block.bits = bitpack::ALL_EQUAL;

    // column is fixed length IFF
    // * it is still a fixed length column
    // * values in a block are of the same length including the last one
    // * values in all blocks have the same length
    fixed_length_ = (fixed_length_ && (block.last_size == block.avg) &&
                     (0 == docs_count_ || block.avg == prev_avg_));
    prev_avg_ = block.avg;
  } else {
    block.bits = packed::maxbits64(begin, end);
    const size_t buf_size =
      packed::bytes_required_64(addr_table_size, block.bits);
    std::memset(ctx_.u64buf, 0, buf_size);
    packed::pack(begin, end, ctx_.u64buf, block.bits);

    data_out.write_bytes(ctx_.u8buf, buf_size);
    fixed_length_ = false;
  }

#ifdef IRESEARCH_DEBUG
  block.size = data_.file.length();
#endif

  if (data_.file.length()) {
    block.data += data_out.file_pointer();

    if (ctx_.cipher) {
      auto offset = data_out.file_pointer();

      auto encrypt_and_copy = [&data_out, cipher = ctx_.cipher, &offset](
                                irs::byte_type* b, size_t len) {
        assert(cipher);

        if (!cipher->encrypt(offset, b, len)) {
          return false;
        }

        data_out.write_bytes(b, len);
        offset += len;
        return true;
      };

      if (!data_.file.visit(encrypt_and_copy)) {
        throw io_error("failed to encrypt columnstore");
      }
    } else {
      data_.file >> data_out;
    }

    data_.stream.seek(0);
    data_.file.reset();
  }

  addr_table_.reset();

  docs_count_ += docs_count;
}

void column::finish(index_output& index_out) {
  assert(id_ < field_limits::invalid());
  assert(ctx_.data_out);

  docs_writer_.finish();
  docs_.stream.flush();

  column_header hdr;
  hdr.docs_count = docs_count_;
  hdr.id = id_;

  memory_index_input in{docs_.file};
  sparse_bitmap_iterator it{&in,
                            {.version = ctx_.version,
                             .track_prev_doc = false,
                             .use_block_index = false,
                             .blocks = {}}};
  if (it.next()) {
    hdr.min = it.value();
  }

  // FIXME(gnusi): how to deal with rollback() and docs_writer_.back()?
  if (docs_count_ && (hdr.min + docs_count_ - doc_limits::min() != pend_)) {
    auto& data_out = *ctx_.data_out;

    // we don't need to store bitmap index in case
    // if every document in a column has a value
    hdr.docs_index = data_out.file_pointer();
    docs_.file >> data_out;
  }

  if (IsNull(name_)) {
    hdr.props |= ColumnProperty::kNoName;
  }

  if (ctx_.cipher) {
    hdr.props |= ColumnProperty::kEncrypt;
  }

  if (docs_writer_.version() == SparseBitmapVersion::kPrevDoc) {
    hdr.props |= ColumnProperty::kPrevDoc;
  }

  if (fixed_length_) {
    if (0 == prev_avg_) {
      hdr.type = ColumnType::kMask;
    } else if (ctx_.consolidation) {
#ifdef IRESEARCH_DEBUG
      // ensure blocks are dense after consolidation
      auto prev = std::begin(blocks_);
      if (prev != std::end(blocks_)) {
        auto next = std::next(prev);

        for (; next != std::end(blocks_); ++next) {
          assert(next->data == prev->size + prev->data);
          prev = next;
        }
      }
#endif
      hdr.type = ColumnType::kDenseFixed;
    } else {
      hdr.type = ColumnType::kFixed;
    }
  }

  irs::write_string(index_out, compression_.name());
  write_header(index_out, hdr);
  write_string(index_out, payload_);
  if (!IsNull(name_)) {
    if (ctx_.cipher) {
      auto name = static_cast<std::string>(name_);
      ctx_.cipher->encrypt(index_out.file_pointer(),
                           reinterpret_cast<byte_type*>(name.data()),
                           name.size());
      irs::write_string(index_out, name);
    } else {
      irs::write_string(index_out, name_);
    }
  } else {
    assert(ColumnProperty::kNoName == (ColumnProperty::kNoName & hdr.props));
  }

  if (hdr.docs_index) {
    // write bitmap index IFF it's really necessary
    write_bitmap_index(index_out, docs_writer_.index());
  }

  if (ColumnType::kSparse == hdr.type) {
    write_blocks_sparse(index_out, blocks_);
  } else if (ColumnType::kMask != hdr.type) {
    index_out.write_long(blocks_.front().avg);
    if (ColumnType::kDenseFixed == hdr.type) {
      index_out.write_long(blocks_.front().data);
    } else {
      assert(ColumnType::kFixed == hdr.type);
      write_blocks_dense(index_out, blocks_);
    }
  }
}

writer::writer(Version version, bool consolidation)
  : dir_{nullptr},
    alloc_{&memory_allocator::global()},
    buf_{std::make_unique<byte_type[]>(column::kBlockSize * sizeof(uint64_t))},
    ver_{version},
    consolidation_{consolidation} {}

void writer::prepare(directory& dir, const segment_meta& meta) {
  columns_.clear();

  auto filename = data_file_name(meta.name);
  auto data_out = dir.create(filename);

  if (!data_out) {
    throw io_error{string_utils::to_string("Failed to create file, path: %s",
                                           filename.c_str())};
  }

  format_utils::write_header(*data_out, kDataFormatName,
                             static_cast<int32_t>(ver_));

  encryption::stream::ptr data_cipher;
  bstring enc_header;
  auto* enc = dir.attributes().encryption();
  const auto encrypt =
    irs::encrypt(filename, *data_out, enc, enc_header, data_cipher);
  assert(!encrypt || (data_cipher && data_cipher->block_size()));
  UNUSED(encrypt);

  // noexcept block
  dir_ = &dir;
  alloc_ = &dir.attributes().allocator();
  data_filename_ = std::move(filename);
  data_out_ = std::move(data_out);
  data_cipher_ = std::move(data_cipher);
}

columnstore_writer::column_t writer::push_column(const column_info& info,
                                                 column_finalizer_f finalizer) {
  // FIXME
  // Since current implementation doesn't support custom compression,
  // we ignore the compression option set in column_info.
  // We can potentially implement compression logic later on,
  // thus we make a column aware of compression algrorithm for further
  // extensibility.
  auto compression =
    irs::type<compression::none>::get(); /* info.compression(); */

  encryption::stream* cipher = info.encryption ? data_cipher_.get() : nullptr;

  auto compressor = compression::get_compressor(compression, info.options);

  if (!compressor) {
    compressor = compression::compressor::identity();
  }

  const auto id = columns_.size();

  if (id >= std::numeric_limits<uint32_t>::max()) {
    throw illegal_state{"Too many columns."};
  }

  // in case of consolidation we write columns one-by-one to
  // ensure that blocks from different columns don't interleave
  if (consolidation_ && id) {
    columns_.back().flush();
  }

  auto& column = columns_.emplace_back(
    column::context{.alloc = alloc_,
                    .data_out = data_out_.get(),
                    .cipher = cipher,
                    .u8buf = buf_.get(),
                    .consolidation = consolidation_,
                    .version = ToSparseBitmapVersion(info)},
    static_cast<field_id>(id), compression, std::move(finalizer),
    std::move(compressor));

  return {id, [&column](doc_id_t doc) -> column_output& {
            // to avoid extra (and useless in our case) check for block index
            // emptiness in 'writer::column::prepare', we disallow passing
            // doc <= doc_limits::invalid() || doc >= doc_limits::eof()
            assert(doc > doc_limits::invalid() && doc < doc_limits::eof());

            column.prepare(doc);
            return column;
          }};
}

bool writer::commit(const flush_state& /*state*/) {
  assert(dir_);

  // remove all empty columns from tail
  while (!columns_.empty() && columns_.back().empty()) {
    columns_.pop_back();
  }

  // remove file if there is no data to write
  if (columns_.empty()) {
    data_out_.reset();

    if (!dir_->remove(data_filename_)) {  // ignore error
      IR_FRMT_ERROR("Failed to remove file, path: %s", data_filename_.c_str());
    }

    return false;  // nothing to flush
  }

  assert(sorted_columns_.empty());
  sorted_columns_.reserve(columns_.size());

  for (auto& column : columns_) {
    column.finalize();  // Flush remain blocks and execute finalizer
    sorted_columns_.emplace_back(&column);
  }

  std::sort(std::begin(sorted_columns_), std::end(sorted_columns_),
            [](const auto* lhs, const auto* rhs) {
              return ::less(lhs->name(), rhs->name());
            });

  // Ensured by `push_column(...)`
  assert(columns_.size() < field_limits::invalid());
  assert(columns_.size() == sorted_columns_.size());
  const field_id count = static_cast<field_id>(columns_.size());

  const std::string_view segment_name{
    data_filename_.data(), data_filename_.size() - kDataFormatExt.size() - 1};
  auto index_filename = index_file_name(segment_name);
  auto index_out = dir_->create(index_filename);

  if (!index_out) {
    throw io_error{string_utils::to_string("Failed to create file, path: %s",
                                           index_filename.c_str())};
  }

  format_utils::write_header(*index_out, kIndexFormatName,
                             static_cast<int32_t>(ver_));

  index_out->write_vint(static_cast<uint32_t>(count));
  for (auto* column : sorted_columns_) {
    column->finish(*index_out);
  }

  format_utils::write_footer(*index_out);
  format_utils::write_footer(*data_out_);

  rollback();

  return true;
}

void writer::rollback() noexcept {
  data_filename_.clear();
  dir_ = nullptr;
  data_out_.reset();  // close output
  columns_.clear();
  sorted_columns_.clear();
}

const column_header* reader::header(field_id field) const {
  auto* column = field >= columns_.size()
                   ? nullptr  // can't find column with the specified identifier
                   : columns_[field];

  if (column) {
    return &down_cast<column_base>(*column).header();
  }

  return nullptr;
}

void reader::prepare_data(const directory& dir, std::string_view filename) {
  auto data_in = dir.open(filename, irs::IOAdvice::RANDOM);

  if (!data_in) {
    throw io_error{string_utils::to_string("Failed to open file, path: %s",
                                           std::string{filename}.c_str())};
  }

  [[maybe_unused]] const auto version = format_utils::check_header(
    *data_in, writer::kDataFormatName, static_cast<int32_t>(Version::kMin),
    static_cast<int32_t>(Version::kMax));

  encryption::stream::ptr cipher;
  auto* enc = dir.attributes().encryption();
  if (irs::decrypt(filename, *data_in, enc, cipher)) {
    assert(cipher && cipher->block_size());
  }

  // since columns data are too large
  // it is too costly to verify checksum of
  // the entire file. here we perform cheap
  // error detection which could recognize
  // some forms of corruption
  [[maybe_unused]] const auto checksum = format_utils::read_checksum(*data_in);

  // noexcept
  data_cipher_ = std::move(cipher);
  data_in_ = std::move(data_in);
}

// FIXME return result???
void reader::prepare_index(const directory& dir, const segment_meta& meta,
                           std::string_view filename,
                           std::string_view data_filename,
                           const options& opts) {
  auto index_in =
    dir.open(filename, irs::IOAdvice::READONCE | irs::IOAdvice::SEQUENTIAL);

  if (!index_in) {
    throw io_error{string_utils::to_string("Failed to open file, path: %s",
                                           std::string{filename}.c_str())};
  }

  const auto checksum = format_utils::checksum(*index_in);

  [[maybe_unused]] const auto version = format_utils::check_header(
    *index_in, writer::kIndexFormatName, static_cast<int32_t>(Version::kMin),
    static_cast<int32_t>(Version::kMax));

  const field_id count = index_in->read_vint();

  decltype(sorted_columns_) sorted_columns;
  sorted_columns.reserve(count);
  decltype(columns_) columns;
  columns.resize(count);

  for (field_id i = 0; i < count; ++i) {
    const auto compression_id = read_string<std::string>(*index_in);
    auto inflater = compression::get_decompressor(compression_id);

    if (!inflater && !compression::exists(compression_id)) {
      throw index_error{string_utils::to_string(
        "Failed to load compression '%s' for column id=" IR_SIZE_T_SPECIFIER "",
        compression_id.c_str(), i)};
    }

    if (inflater && !inflater->prepare(*index_in)) {  // FIXME or data_in???
      throw index_error{
        string_utils::to_string("Failed to prepare compression '%s' for "
                                "column id=" IR_SIZE_T_SPECIFIER "",
                                compression_id.c_str(), i)};
    }

    column_header hdr = read_header(*index_in);

    const bool encrypted = is_encrypted(hdr);

    if (encrypted && !data_cipher_) {
      throw index_error{string_utils::to_string(
        "Failed to load encrypted column id=" IR_SIZE_T_SPECIFIER
        " without a cipher",
        i)};
    }

    if (ColumnType::kMask != hdr.type && 0 == hdr.docs_count) {
      throw index_error{
        string_utils::to_string("Failed to load column id=" IR_SIZE_T_SPECIFIER
                                ", only mask column may be empty",
                                i)};
    }

    if (hdr.id >= std::numeric_limits<uint32_t>::max() || hdr.id >= count) {
      throw index_error{
        string_utils::to_string("Failed to load column id=" IR_SIZE_T_SPECIFIER
                                ", invalid ordinal position",
                                i)};
    }

    auto payload = read_string<bstring>(*index_in);

    std::optional<std::string> name;
    if (ColumnProperty::kNoName != (hdr.props & ColumnProperty::kNoName)) {
      [[maybe_unused]] const auto offset = index_in->file_pointer();

      name = irs::read_string<std::string>(*index_in);

      if (encrypted) {
        assert(data_cipher_);
        data_cipher_->decrypt(
          offset, reinterpret_cast<byte_type*>(name->data()), name->size());
      }
    }

    auto index = hdr.docs_index ? read_bitmap_index(*index_in) : column_index{};

    if (const size_t idx = static_cast<size_t>(hdr.type);
        IRS_LIKELY(idx < std::size(kFactories))) {
      auto column = kFactories[idx](
        std::move(name), std::move(payload), std::move(hdr), std::move(index),
        *index_in, *data_in_, std::move(inflater), data_cipher_.get());
      assert(column);

      if (!sorted_columns.empty() &&
          ::less(column->name(), sorted_columns.back()->name())) {
        throw irs::index_error(irs::string_utils::to_string(
          "invalid column order in segment '%s'", meta.name.c_str()));
      }

      assert(hdr.id < columns.size());
      columns[hdr.id] = column.get();
      sorted_columns.emplace_back(std::move(column));
    } else {
      throw index_error{
        string_utils::to_string("Failed to load column id=" IR_SIZE_T_SPECIFIER
                                " , got invalid type=%u",
                                i, static_cast<uint32_t>(hdr.type))};
    }
  }
  if (opts.warmup_column) {
    index_input::ptr direct_data_input;
    auto memory_accounting =
      opts.pinned_memory ? opts.pinned_memory : NoopMemoryAccounter;
    for (size_t i = 0; i < sorted_columns.size(); ++i) {
      auto cb = static_cast<column_base*>(sorted_columns[i].get());
      if (opts.warmup_column(*cb)) {
        if (!direct_data_input) {
          direct_data_input = dir.open(data_filename, IOAdvice::DIRECT_READ);
          if (!direct_data_input) {
            IR_FRMT_WARN(
              "Failed to open direct access file, path: %s. Columns buffering "
              "stopped.",
              data_filename.data());
            break;
          }
        }
        IR_FRMT_TRACE("Making buffered: %llu", cb->header().id);
        cb->make_buffered(*direct_data_input, memory_accounting,
                          std::span(sorted_columns.data() + i + 1,
                                    sorted_columns.size() - i - 1));
        IR_FRMT_TRACE("Finished buffered: %llu", cb->header().id);
      }
    }
  }

  format_utils::check_footer(*index_in, checksum);

  // noexcept block
  columns_ = std::move(columns);
  sorted_columns_ = std::move(sorted_columns);

  assert(columns_.size() == sorted_columns_.size());
}

bool reader::prepare(const directory& dir, const segment_meta& meta,
                     const options& opts) {
  bool exists;
  const auto data_filename = data_file_name(meta.name);

  if (!dir.exists(exists, data_filename)) {
    throw io_error{string_utils::to_string(
      "failed to check existence of file, path: %s", data_filename.c_str())};
  }

  if (!exists) {
    // possible that the file does not exist
    // since columnstore is optional
    return false;
  }

  prepare_data(dir, data_filename);
  assert(data_in_);

  const auto index_filename = index_file_name(meta.name);

  if (!dir.exists(exists, index_filename)) {
    throw io_error{string_utils::to_string(
      "failed to check existence of file, path: %s", index_filename.c_str())};
  }

  if (!exists) {
    // more likely index is currupted
    throw index_error{string_utils::to_string(
      "columnstore index file '%s' is missing", index_filename.c_str())};
  }

  prepare_index(dir, meta, index_filename, data_filename, opts);

  return true;
}

bool reader::visit(const column_visitor_f& visitor) const {
  for (const auto& column : sorted_columns_) {
    assert(column);
    if (!visitor(*column)) {
      return false;
    }
  }
  return true;
}

irs::columnstore_writer::ptr make_writer(Version version, bool consolidation) {
  return std::make_unique<writer>(version, consolidation);
}

irs::columnstore_reader::ptr make_reader() {
  return std::make_unique<reader>();
}

}  // namespace columnstore2
}  // namespace iresearch
