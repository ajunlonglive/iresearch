////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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

#ifndef IRESEARCH_LZ4COMPRESSION_H
#define IRESEARCH_LZ4COMPRESSION_H

#include "string.hpp"
#include "compression.hpp"
#include "noncopyable.hpp"

#include <memory>

namespace iresearch {
namespace compression {

struct LZ4_stream_deleter {
  void operator()(void* p) noexcept;
};

struct LZ4_streamDecode_deleter {
  void operator()(void* p) noexcept;
};

typedef std::unique_ptr<void, LZ4_stream_deleter> lz4stream;
typedef std::unique_ptr<void, LZ4_streamDecode_deleter> lz4stream_decode;

lz4stream lz4_make_stream();
lz4stream_decode lz4_make_stream_decode();

struct lz4 {
  static constexpr std::string_view type_name() noexcept {
    return "iresearch::compression::lz4";
  }

  class lz4compressor final : public compression::compressor {
   public:
    explicit lz4compressor(int acceleration = 0) noexcept
      : acceleration_(acceleration) {}

    int acceleration() const noexcept { return acceleration_; }

    virtual bytes_view compress(byte_type* src, size_t size,
                               bstring& out) override
      IRESEARCH_ATTRIBUTE_NONNULL();

   private:
    const int acceleration_{0};  // 0 - default acceleration
  };

  class lz4decompressor final : public compression::decompressor {
   public:
    virtual bytes_view decompress(const byte_type* src, size_t src_size,
                                 byte_type* dst, size_t dst_size) override
      IRESEARCH_ATTRIBUTE_NONNULL();
  };

  static void init();
  static compression::compressor::ptr compressor(const options& opts);
  static compression::decompressor::ptr decompressor();
};  // lz4basic

}  // namespace compression
}  // namespace iresearch

#endif
