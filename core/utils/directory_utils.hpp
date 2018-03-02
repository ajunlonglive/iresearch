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

#ifndef IRESEARCH_DIRECTORY_UTILS_H
#define IRESEARCH_DIRECTORY_UTILS_H

#include "shared.hpp"
#include "store/data_input.hpp"
#include "store/data_output.hpp"
#include "store/directory.hpp"
#include "store/directory_cleaner.hpp"

NS_ROOT

class format;
class index_meta;
struct segment_meta;

NS_BEGIN(directory_utils)

// return a reference to a file or empty() if not found
IRESEARCH_API index_file_refs::ref_t reference(
  directory& dir,
  const std::string& name,
  bool include_missing = false
);

// return success, visitor gets passed references to files retrieved from source
IRESEARCH_API bool reference(
  directory& dir,
  const std::function<const std::string*()>& source,
  const std::function<bool(index_file_refs::ref_t&& ref)>& visitor,
  bool include_missing = false
);

// return success, visitor gets passed references to files registered with index_meta
IRESEARCH_API bool reference(
  directory& dir,
  const index_meta& meta,
  const std::function<bool(index_file_refs::ref_t&& ref)>& visitor,
  bool include_missing = false
);

// return success, visitor gets passed references to files registered with segment_meta
IRESEARCH_API bool reference(
  directory& dir,
  const segment_meta& meta,
  const std::function<bool(index_file_refs::ref_t&& ref)>& visitor,
  bool include_missing = false
);

// remove all (tracked and non-tracked) files if they are unreferenced
IRESEARCH_API void remove_all_unreferenced(directory& dir);

// remove tracked files if they are unreferenced and not part of the latest segments
IRESEARCH_API directory_cleaner::removal_acceptor_t remove_except_current_segments(
  const directory& dir, format& codec
);

NS_END

//////////////////////////////////////////////////////////////////////////////
/// @class tracking_directory
/// @brief track files created/opened via file names
//////////////////////////////////////////////////////////////////////////////

struct IRESEARCH_API tracking_directory: public directory {
  typedef std::unordered_set<std::string> file_set;

  // @param track_open - track file refs for calls to open(...)
  tracking_directory(directory& impl, bool track_open = false);
  virtual ~tracking_directory();
  directory& operator*() NOEXCEPT;
  using directory::attributes;
  virtual attribute_store& attributes() NOEXCEPT override;
  virtual void close() NOEXCEPT override;
  virtual index_output::ptr create(const std::string& name) NOEXCEPT override;
  virtual bool exists(
    bool& result, const std::string& name
  ) const NOEXCEPT override;
  virtual bool length(
    uint64_t& result, const std::string& name
  ) const NOEXCEPT override;
  virtual index_lock::ptr make_lock(const std::string& name) NOEXCEPT override;
  virtual bool mtime(
    std::time_t& result, const std::string& name
  ) const NOEXCEPT override;
  virtual index_input::ptr open(
    const std::string& name,
    IOAdvice advice
  ) const NOEXCEPT override;
  virtual bool remove(const std::string& name) NOEXCEPT override;
  virtual bool rename(
    const std::string& src, const std::string& dst
  ) NOEXCEPT override;
  bool swap_tracked(file_set& other) NOEXCEPT;
  bool swap_tracked(tracking_directory& other) NOEXCEPT;
  virtual bool sync(const std::string& name) NOEXCEPT override;
  virtual bool visit(const visitor_f& visitor) const override;

 private:
  IRESEARCH_API_PRIVATE_VARIABLES_BEGIN
  mutable file_set files_;
  directory& impl_;
  bool track_open_;
  IRESEARCH_API_PRIVATE_VARIABLES_END
};

//////////////////////////////////////////////////////////////////////////////
/// @class ref_tracking_directory
/// @brief track files created/opened via file refs instead of file names
//////////////////////////////////////////////////////////////////////////////

struct IRESEARCH_API ref_tracking_directory: public directory {
 public:
  DECLARE_UNIQUE_PTR(ref_tracking_directory);
  // @param track_open - track file refs for calls to open(...)
  ref_tracking_directory(directory& impl, bool track_open = false);
  ref_tracking_directory(ref_tracking_directory&& other) NOEXCEPT;
  virtual ~ref_tracking_directory();
  directory& operator*() NOEXCEPT;
  using directory::attributes;
  virtual attribute_store& attributes() NOEXCEPT override;
  void clear_refs() const NOEXCEPT;
  virtual void close() NOEXCEPT override;
  virtual index_output::ptr create(const std::string &name) NOEXCEPT override;
  virtual bool exists(
    bool& result, const std::string& name
  ) const NOEXCEPT override;
  virtual bool length(
    uint64_t& result, const std::string& name
  ) const NOEXCEPT override;
  virtual index_lock::ptr make_lock(const std::string& name) NOEXCEPT override;
  virtual bool mtime(
    std::time_t& result, const std::string& name
  ) const NOEXCEPT override;
  virtual index_input::ptr open(
    const std::string& name,
    IOAdvice advice
  ) const NOEXCEPT override;
  virtual bool remove(const std::string& name) NOEXCEPT override;
  virtual bool rename(const std::string& src, const std::string& dst) NOEXCEPT override;
  virtual bool sync(const std::string& name) NOEXCEPT override;
  virtual bool visit(const visitor_f& visitor) const override;
  bool visit_refs(const std::function<bool(const index_file_refs::ref_t& ref)>& visitor) const;

 private:
  IRESEARCH_API_PRIVATE_VARIABLES_BEGIN
  index_file_refs::attribute_t& attribute_;
  directory& impl_;
  mutable std::mutex mutex_; // for use with refs_
  mutable std::unordered_map<string_ref, index_file_refs::ref_t> refs_;
  bool track_open_;
  IRESEARCH_API_PRIVATE_VARIABLES_END
};

NS_END

#endif
