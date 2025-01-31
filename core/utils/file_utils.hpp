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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <fcntl.h>  // open/_wopen

#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>

#include "shared.hpp"
#include "string.hpp"
#include "utils/bit_utils.hpp"

#ifdef _WIN32
#include <io.h>  // _close
#include <tchar.h>
#define file_blksize_t \
  uint32_t  // DWORD (same as GetDriveGeometry(...)
            // DISK_GEOMETRY::BytesPerSector)
#define file_path_delimiter L'\\'
#define file_stat_t struct _stat64
#define mode_t unsigned short

#define posix_create _wcreat
#define posix_open _wopen
#define posix_close _close
#define file_fstat _fstat64
#define file_stat _wstat64

#define handle_cast(f) f
#define IR_WSTR(x) L##x  // cannot use _T(...) macro when _MBCS is defined
#define IR_DEVNULL IR_WSTR("NUL:")

#define IR_FADVICE_NORMAL 0
#define IR_FADVICE_SEQUENTIAL FILE_FLAG_SEQUENTIAL_SCAN
#define IR_FADVICE_RANDOM FILE_FLAG_RANDOM_ACCESS
#define IR_FADVICE_DONTNEED 0
#define IR_FADVICE_NOREUSE 0
#else
#include <sys/types.h>  // for blksize_t
#include <unistd.h>     // close
#define file_blksize_t blksize_t
#define file_path_delimiter '/'
#define file_stat_t struct stat

#define file_stat stat
#define file_fstat fstat
#define posix_create creat
#define posix_open open
#define posix_close close

#define handle_cast(f) static_cast<int>(reinterpret_cast<size_t>(f))
#define IR_DEVNULL "/dev/null"
#ifndef __APPLE__
#define IR_FADVICE_NORMAL POSIX_FADV_NORMAL
#define IR_FADVICE_SEQUENTIAL POSIX_FADV_SEQUENTIAL
#define IR_FADVICE_RANDOM POSIX_FADV_RANDOM
#define IR_FADVICE_DONTNEED POSIX_FADV_DONTNEED
#define IR_FADVICE_NOREUSE POSIX_FADV_NOREUSE
#else
#define IR_FADVICE_NORMAL 0
#define IR_FADVICE_SEQUENTIAL 0
#define IR_FADVICE_RANDOM 0
#define IR_FADVICE_DONTNEED 0
#define IR_FADVICE_NOREUSE 0
#endif
#endif

using path_char_t = std::filesystem::path::value_type;
#define file_path_t path_char_t*

namespace iresearch::file_utils {

// -----------------------------------------------------------------------------
// --SECTION--                                                         lock file
// -----------------------------------------------------------------------------

struct lock_file_deleter {
  void operator()(void* handle) const;
};  // lock_file_deleter

typedef std::unique_ptr<void, lock_file_deleter> lock_handle_t;

lock_handle_t create_lock_file(const file_path_t file);
bool verify_lock_file(const file_path_t file);

// -----------------------------------------------------------------------------
// --SECTION--                                                             stats
// -----------------------------------------------------------------------------

bool absolute(bool& result, const file_path_t path) noexcept;

bool block_size(file_blksize_t& result, const file_path_t file) noexcept;
bool block_size(file_blksize_t& result, void* fd) noexcept;
bool byte_size(uint64_t& result, const file_path_t file) noexcept;
bool byte_size(uint64_t& result, int fd) noexcept;
bool byte_size(uint64_t& result, void* fd) noexcept;

bool exists(bool& result, const file_path_t file) noexcept;
bool exists_directory(bool& result, const file_path_t file) noexcept;
bool exists_file(bool& result, const file_path_t file) noexcept;

bool mtime(time_t& result, const file_path_t file) noexcept;

enum class OpenMode : uint16_t { Invalid = 0, Read = 1, Write = 2, Direct = 4 };

ENABLE_BITMASK_ENUM(OpenMode);

struct file_deleter {
  void operator()(void* f) const noexcept;
};  // file_deleter

typedef std::unique_ptr<void, file_deleter> handle_t;

handle_t open(const file_path_t path, OpenMode mode, int advice) noexcept;
handle_t open(void* file, OpenMode mode, int advice) noexcept;

bool mkdir(const file_path_t path,
           bool createNew) noexcept;  // recursive directory creation

bool move(const file_path_t src_path, const file_path_t dst_path) noexcept;

size_t fread(void* fd, void* buf, size_t size);
size_t fwrite(void* fd, const void* buf, size_t size);
FORCE_INLINE bool write(void* fd, const void* buf, size_t size) {
  return fwrite(fd, buf, size) == size;
}
int fseek(void* fd, long pos, int origin);
int ferror(void*);
long ftell(void* fd);

struct path_parts_t {
  using ref_t = std::basic_string_view<path_char_t>;
  ref_t basename;   // path component after the last path delimiter (ref_t{}
                    // if not present)
  ref_t dirname;    // path component before the last path delimiter (ref_t{}
                    // if not present)
  ref_t extension;  // basename extension (ref_t{} if not present)
  ref_t stem;       // basename without extension (ref_t{} if not present)
};

path_parts_t path_parts(const file_path_t path) noexcept;

bool read_cwd(std::basic_string<path_char_t>& result) noexcept;

void ensure_absolute(std::filesystem::path& path);

bool remove(const file_path_t path) noexcept;

bool set_cwd(const file_path_t path) noexcept;

bool visit_directory(const file_path_t name,
                     const std::function<bool(const file_path_t name)>& visitor,
                     bool include_dot_dir = true);

bool file_sync(const file_path_t name) noexcept;
bool file_sync(int fd) noexcept;

}  // namespace iresearch::file_utils
