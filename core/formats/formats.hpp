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

#ifndef IRESEARCH_FORMAT_H
#define IRESEARCH_FORMAT_H

#include <absl/container/flat_hash_set.h>

#include <function2/function2.hpp>

#include "formats/seek_cookie.hpp"
#include "index/column_info.hpp"
#include "index/index_features.hpp"
#include "index/index_meta.hpp"
#include "index/iterators.hpp"
#include "shared.hpp"
#include "store/data_output.hpp"
#include "store/directory.hpp"
#include "utils/attribute_provider.hpp"
#include "utils/automaton_decl.hpp"
#include "utils/io_utils.hpp"
#include "utils/string.hpp"
#include "utils/type_info.hpp"

namespace iresearch {

struct segment_meta;
struct field_meta;
struct flush_state;
struct reader_state;
struct index_output;
struct data_input;
struct index_input;
struct postings_writer;

using document_mask = absl::flat_hash_set<doc_id_t>;
using doc_map = std::vector<doc_id_t>;
using callback_f = std::function<bool(doc_iterator&)>;
// should never throw as may be used in dtors
using memory_accounting_f = fu2::function<bool(int64_t) noexcept>;

constexpr bool NoopMemoryAccounter(int64_t) noexcept { return true; }

//////////////////////////////////////////////////////////////////////////////
/// @class term_meta
/// @brief represents metadata associated with the term
//////////////////////////////////////////////////////////////////////////////
struct term_meta : attribute {
  static constexpr std::string_view type_name() noexcept {
    return "iresearch::term_meta";
  }

  void clear() noexcept {
    docs_count = 0;
    freq = 0;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief how many documents a particular term contains
  //////////////////////////////////////////////////////////////////////////////
  uint32_t docs_count = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief how many times a particular term occur in documents
  //////////////////////////////////////////////////////////////////////////////
  uint32_t freq = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct postings_writer
////////////////////////////////////////////////////////////////////////////////
struct postings_writer : attribute_provider {
  using ptr = std::unique_ptr<postings_writer>;

  class releaser {
   public:
    explicit releaser(postings_writer* owner = nullptr) noexcept
      : owner_(owner) {}

    inline void operator()(term_meta* meta) const noexcept;

   private:
    postings_writer* owner_;
  };  // releaser

  typedef std::unique_ptr<term_meta, releaser> state;

  virtual ~postings_writer() = default;
  /* out - corresponding terms utils/utstream */
  virtual void prepare(index_output& out, const flush_state& state) = 0;
  virtual void begin_field(IndexFeatures features) = 0;
  virtual state write(doc_iterator& docs) = 0;
  virtual void begin_block() = 0;
  virtual void encode(data_output& out, const term_meta& state) = 0;
  virtual void end() = 0;

 protected:
  friend struct term_meta;

  state make_state(term_meta& meta) noexcept {
    return state(&meta, releaser(this));
  }

  virtual void release(term_meta* meta) noexcept = 0;
};

void postings_writer::releaser::operator()(term_meta* meta) const noexcept {
  assert(owner_ && meta);
  owner_->release(meta);
}

////////////////////////////////////////////////////////////////////////////////
/// @struct field_writer
////////////////////////////////////////////////////////////////////////////////
struct field_writer {
  using ptr = std::unique_ptr<field_writer>;

  virtual ~field_writer() = default;
  virtual void prepare(const flush_state& state) = 0;
  virtual void write(std::string_view name, IndexFeatures index_features,
                     const std::map<type_info::type_id, field_id>& features,
                     term_iterator& data) = 0;
  virtual void end() = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct postings_reader
////////////////////////////////////////////////////////////////////////////////
struct postings_reader {
  using ptr = std::unique_ptr<postings_reader>;
  using term_provider_f = std::function<const term_meta*()>;

  virtual ~postings_reader() = default;

  //////////////////////////////////////////////////////////////////////////////
  /// @arg in - corresponding stream
  /// @arg features - the set of features available for segment
  //////////////////////////////////////////////////////////////////////////////
  virtual void prepare(index_input& in, const reader_state& state,
                       IndexFeatures features) = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief parses input block "in" and populate "attrs" collection with
  /// attributes
  /// @returns number of bytes read from in
  //////////////////////////////////////////////////////////////////////////////
  virtual size_t decode(const byte_type* in, IndexFeatures features,
                        term_meta& state) = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @returns a document iterator for a specified 'cookie' and 'features'
  //////////////////////////////////////////////////////////////////////////////
  virtual doc_iterator::ptr iterator(IndexFeatures field_features,
                                     IndexFeatures required_features,
                                     const term_meta& meta) = 0;

  virtual doc_iterator::ptr wanderator(IndexFeatures field_features,
                                       IndexFeatures required_features,
                                       const term_meta& meta) = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief evaluates a union of all docs denoted by attribute supplied via a
  ///        speciified 'provider'. Each doc is represented by a bit in a
  ///        specified 'bitset'.
  /// @returns a number of bits set
  /// @note it's up to the caller to allocate enough space for a bitset
  /// @note this API is experimental
  //////////////////////////////////////////////////////////////////////////////
  virtual size_t bit_union(IndexFeatures field_features,
                           const term_provider_f& provider, size_t* set) = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct basic_term_reader
////////////////////////////////////////////////////////////////////////////////
struct basic_term_reader : public attribute_provider {
  virtual ~basic_term_reader() = default;

  virtual term_iterator::ptr iterator() const = 0;

  // returns field metadata
  virtual const field_meta& meta() const = 0;

  // least significant term
  virtual bytes_view(min)() const = 0;

  // most significant term
  virtual bytes_view(max)() const = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @enum SeekMode
/// @brief an expected usage pattern of seek_term_iterator
////////////////////////////////////////////////////////////////////////////////
enum class SeekMode : uint32_t {
  //////////////////////////////////////////////////////////////////////////////
  /// @brief default mode, e.g. multiple consequent seeks are expected
  //////////////////////////////////////////////////////////////////////////////
  NORMAL = 0,

  //////////////////////////////////////////////////////////////////////////////
  /// @brief only random exact seeks are supported
  //////////////////////////////////////////////////////////////////////////////
  RANDOM_ONLY
};

////////////////////////////////////////////////////////////////////////////////
/// @struct term_reader
////////////////////////////////////////////////////////////////////////////////
struct term_reader : public attribute_provider {
  using ptr = std::unique_ptr<term_reader>;
  using cookie_provider = std::function<const seek_cookie*()>;

  virtual ~term_reader() = default;

  //////////////////////////////////////////////////////////////////////////////
  /// @param mode seek mode for term iterator
  /// @returns an iterator over terms for a field
  //////////////////////////////////////////////////////////////////////////////
  virtual seek_term_iterator::ptr iterator(SeekMode mode) const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @returns an intersection of a specified automaton and term reader
  //////////////////////////////////////////////////////////////////////////////
  virtual seek_term_iterator::ptr iterator(
    automaton_table_matcher& matcher) const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief evaluates a union of all docs denoted by cookies supplied via a
  ///        speciified 'provider'. Each doc is represented by a bit in a
  ///        specified 'bitset'.
  /// @returns a number of bits set
  /// @note it's up to the caller to allocate enough space for a bitset
  /// @note this API is experimental
  //////////////////////////////////////////////////////////////////////////////
  virtual size_t bit_union(const cookie_provider& provider,
                           size_t* bitset) const = 0;

  virtual doc_iterator::ptr postings(const seek_cookie& cookie,
                                     IndexFeatures features) const = 0;

  virtual doc_iterator::ptr wanderator(const seek_cookie& cookie,
                                       IndexFeatures features) const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @returns field metadata
  //////////////////////////////////////////////////////////////////////////////
  virtual const field_meta& meta() const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @returns total number of terms
  //////////////////////////////////////////////////////////////////////////////
  virtual size_t size() const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @returns total number of documents with at least 1 term in a field
  //////////////////////////////////////////////////////////////////////////////
  virtual uint64_t docs_count() const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @returns the least significant term
  //////////////////////////////////////////////////////////////////////////////
  virtual bytes_view(min)() const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @returns the most significant term
  //////////////////////////////////////////////////////////////////////////////
  virtual bytes_view(max)() const = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct field_reader
////////////////////////////////////////////////////////////////////////////////
struct field_reader {
  using ptr = std::unique_ptr<field_reader>;

  virtual ~field_reader() = default;

  virtual void prepare(const directory& dir, const segment_meta& meta,
                       const document_mask& mask) = 0;

  virtual const term_reader* field(std::string_view field) const = 0;
  virtual field_iterator::ptr iterator() const = 0;
  virtual size_t size() const = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct column_output
////////////////////////////////////////////////////////////////////////////////
struct column_output : data_output {
  // resets stream to previous persisted state
  virtual void reset() = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct columnstore_writer
////////////////////////////////////////////////////////////////////////////////
struct columnstore_writer {
  using ptr = std::unique_ptr<columnstore_writer>;

  // NOTE: doc > type_limits<type_t::doc_id_t>::invalid() && doc <
  // type_limits<type_t::doc_id_t>::eof()
  using values_writer_f = std::function<column_output&(doc_id_t doc)>;

  // Finalizer can be used to assign name and payload to a column.
  // Returned `std::string_view` must be valid during `commit(...)`.
  using column_finalizer_f =
    fu2::unique_function<std::string_view(bstring& out)>;

  typedef std::pair<field_id, values_writer_f> column_t;

  virtual ~columnstore_writer() = default;

  virtual void prepare(directory& dir, const segment_meta& meta) = 0;
  virtual column_t push_column(const column_info& info,
                               column_finalizer_f header_writer) = 0;
  virtual void rollback() noexcept = 0;
  virtual bool commit(
    const flush_state& state) = 0;  // @return was anything actually flushed
};                                  // columnstore_writer

}  // namespace iresearch

namespace iresearch {

enum class ColumnHint : uint32_t {
  // Nothing special
  kNormal = 0,
  // Open iterator for conosolidation
  kConsolidation = 1,
  // Reading payload isn't necessary
  kMask = 2,
  // Allow accessing prev document
  kPrevDoc = 4
};

ENABLE_BITMASK_ENUM(ColumnHint);

struct column_reader {
  virtual ~column_reader() = default;

  // Returns column id.
  virtual field_id id() const = 0;

  // Returns optional column name.
  virtual std::string_view name() const = 0;

  // Returns column header.
  virtual bytes_view payload() const = 0;

  // FIXME(gnusi): implement mode
  //  Returns the corresponding column iterator.
  //  If the column implementation supports document payloads then it
  //  can be accessed via the 'payload' attribute.
  virtual doc_iterator::ptr iterator(ColumnHint hint) const = 0;

  // Returns total number of columns.
  virtual doc_id_t size() const = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct columnstore_reader
////////////////////////////////////////////////////////////////////////////////
struct columnstore_reader {
  using ptr = std::unique_ptr<columnstore_reader>;

  using column_visitor_f = std::function<bool(const column_reader&)>;

  struct options {
    // allows to select "hot" columns
    column_visitor_f warmup_column;
    // allows to restrict "hot" columns memory usage
    memory_accounting_f pinned_memory;
  };

  virtual ~columnstore_reader() = default;

  // Returns true if conlumnstore is present in a segment, false - otherwise.
  // May throw `io_error` or `index_error`.
  virtual bool prepare(const directory& dir, const segment_meta& meta,
                       const options& opts = options{}) = 0;

  virtual bool visit(const column_visitor_f& visitor) const = 0;

  virtual const column_reader* column(field_id field) const = 0;

  // Returns total number of columns.
  virtual size_t size() const = 0;
};

}  // namespace iresearch

namespace iresearch {

////////////////////////////////////////////////////////////////////////////////
/// @struct document_mask_writer
////////////////////////////////////////////////////////////////////////////////
struct document_mask_writer {
  using ptr = memory::managed_ptr<document_mask_writer>;

  virtual ~document_mask_writer() = default;

  virtual std::string filename(const segment_meta& meta) const = 0;

  virtual void write(directory& dir, const segment_meta& meta,
                     const document_mask& docs_mask) = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct document_mask_reader
////////////////////////////////////////////////////////////////////////////////
struct document_mask_reader {
  using ptr = memory::managed_ptr<document_mask_reader>;

  virtual ~document_mask_reader() = default;

  /// @returns true if there are any deletes in a segment,
  ///          false - otherwise
  /// @throws io_error
  /// @throws index_error
  virtual bool read(const directory& dir, const segment_meta& meta,
                    document_mask& docs_mask) = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct segment_meta_writer
////////////////////////////////////////////////////////////////////////////////
struct segment_meta_writer {
  using ptr = memory::managed_ptr<segment_meta_writer>;

  virtual ~segment_meta_writer() = default;

  virtual void write(directory& dir, std::string& filename,
                     const segment_meta& meta) = 0;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct segment_meta_reader
////////////////////////////////////////////////////////////////////////////////
struct segment_meta_reader {
  using ptr = memory::managed_ptr<segment_meta_reader>;

  virtual ~segment_meta_reader() = default;

  virtual void read(const directory& dir, segment_meta& meta,
                    std::string_view filename = {}) = 0;  // null == use meta
};                                                        // segment_meta_reader

////////////////////////////////////////////////////////////////////////////////
/// @struct index_meta_writer
////////////////////////////////////////////////////////////////////////////////
struct index_meta_writer {
  using ptr = std::unique_ptr<index_meta_writer>;

  virtual ~index_meta_writer() = default;
  virtual std::string filename(const index_meta& meta) const = 0;
  virtual bool prepare(directory& dir, index_meta& meta) = 0;
  virtual bool commit() = 0;
  virtual void rollback() noexcept = 0;

 protected:
  static void complete(index_meta& meta) noexcept;
  static void prepare(index_meta& meta) noexcept;
};

////////////////////////////////////////////////////////////////////////////////
/// @struct index_meta_reader
////////////////////////////////////////////////////////////////////////////////
struct index_meta_reader {
  using ptr = memory::managed_ptr<index_meta_reader>;

  virtual ~index_meta_reader() = default;

  virtual bool last_segments_file(const directory& dir,
                                  std::string& name) const = 0;

  virtual void read(const directory& dir, index_meta& meta,
                    std::string_view filename = {}) = 0;  // null == use meta

 protected:
  static void complete(index_meta& meta, uint64_t generation, uint64_t counter,
                       index_meta::index_segments_t&& segments,
                       bstring* payload_buf);
};

////////////////////////////////////////////////////////////////////////////////
/// @struct format
////////////////////////////////////////////////////////////////////////////////
class format {
 public:
  using ptr = std::shared_ptr<const format>;

  explicit format(const type_info& type) noexcept : type_(type) {}
  virtual ~format() = default;

  virtual index_meta_writer::ptr get_index_meta_writer() const = 0;
  virtual index_meta_reader::ptr get_index_meta_reader() const = 0;

  virtual segment_meta_writer::ptr get_segment_meta_writer() const = 0;
  virtual segment_meta_reader::ptr get_segment_meta_reader() const = 0;

  virtual document_mask_writer::ptr get_document_mask_writer() const = 0;
  virtual document_mask_reader::ptr get_document_mask_reader() const = 0;

  virtual field_writer::ptr get_field_writer(bool consolidation) const = 0;
  virtual field_reader::ptr get_field_reader() const = 0;

  virtual columnstore_writer::ptr get_columnstore_writer(
    bool consolidation) const = 0;
  virtual columnstore_reader::ptr get_columnstore_reader() const = 0;

  const type_info& type() const { return type_; }

 private:
  type_info type_;
};

}  // namespace iresearch

namespace iresearch {

struct flush_state {
  directory* dir{};
  const doc_map* docmap{};
  const std::set<type_info::type_id>* features{};  // segment features
  std::string_view name;                           // segment name
  size_t doc_count;
  IndexFeatures index_features{IndexFeatures::NONE};  // segment index features
};

struct reader_state {
  const directory* dir;
  const segment_meta* meta;
};

class formats {
 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief checks whether a format with the specified name is registered
  ////////////////////////////////////////////////////////////////////////////////
  static bool exists(std::string_view name, bool load_library = true);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find a format by name, or nullptr if not found
  ///        indirect call to <class>::make(...)
  ///        NOTE: make(...) MUST be defined in CPP to ensire proper code scope
  //////////////////////////////////////////////////////////////////////////////
  static format::ptr get(std::string_view name, std::string_view module = {},
                         bool load_library = true) noexcept;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief for static lib reference all known formats in lib
  ///        for shared lib NOOP
  ///        no explicit call of fn is required, existence of fn is sufficient
  ////////////////////////////////////////////////////////////////////////////////
  static void init();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief load all formats from plugins directory
  ////////////////////////////////////////////////////////////////////////////////
  static void load_all(std::string_view path);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief visit all loaded formats, terminate early if visitor returns false
  ////////////////////////////////////////////////////////////////////////////////
  static bool visit(const std::function<bool(std::string_view)>& visitor);

 private:
  formats() = delete;
};

class format_registrar {
 public:
  format_registrar(const type_info& type, std::string_view module,
                   format::ptr (*factory)(), const char* source = nullptr);

  operator bool() const noexcept { return registered_; }

 private:
  bool registered_;
};

#define REGISTER_FORMAT__(format_name, mudule_name, line, source)           \
  static ::iresearch::format_registrar format_registrar##_##line(           \
    ::iresearch::type<format_name>::get(), mudule_name, &format_name::make, \
    source)
#define REGISTER_FORMAT_EXPANDER__(format_name, mudule_name, file, line) \
  REGISTER_FORMAT__(format_name, mudule_name, line, file ":" TOSTRING(line))
#define REGISTER_FORMAT_MODULE(format_name, module_name) \
  REGISTER_FORMAT_EXPANDER__(format_name, module_name, __FILE__, __LINE__)
#define REGISTER_FORMAT(format_name) \
  REGISTER_FORMAT_MODULE(format_name, std::string_view{})

}  // namespace iresearch

#endif
