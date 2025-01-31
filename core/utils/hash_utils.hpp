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

#ifndef IRESEARCH_HASH_UTILS_H
#define IRESEARCH_HASH_UTILS_H

#include <absl/hash/hash.h>
#include <frozen/string.h>

#include "shared.hpp"
#include "string.hpp"

// -----------------------------------------------------------------------------
// --SECTION--                                                        hash utils
// -----------------------------------------------------------------------------

namespace iresearch {

FORCE_INLINE size_t hash_combine(size_t seed, size_t v) noexcept {
  return seed ^ (v + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

template<typename T>
FORCE_INLINE size_t
hash_combine(size_t seed, T const& v) noexcept(noexcept(std::hash<T>()(v))) {
  return hash_combine(seed, std::hash<T>()(v));
}

template<typename Elem>
class hashed_basic_string_view : public std::basic_string_view<Elem> {
 public:
  using base_t = std::basic_string_view<Elem>;

  hashed_basic_string_view(size_t hash, base_t ref) noexcept
    : base_t(ref), hash_(hash) {}

  hashed_basic_string_view(size_t hash, base_t ref, size_t size) noexcept
    : base_t(ref, size), hash_(hash) {}

  hashed_basic_string_view(size_t hash, typename base_t::pointer ptr) noexcept
    : base_t(ptr), hash_(hash) {}

  hashed_basic_string_view(size_t hash, typename base_t::pointer ptr,
                           size_t size) noexcept
    : base_t(ptr, size), hash_(hash) {}

  size_t hash() const noexcept { return hash_; }

 private:
  size_t hash_;
};

template<typename Elem,
         typename Hasher = std::hash<std::basic_string_view<Elem>>>
hashed_basic_string_view<Elem> make_hashed_ref(
  std::basic_string_view<Elem> ref, const Hasher& hasher = Hasher()) {
  return {hasher(ref), ref};
}

template<typename Elem,
         typename Hasher = std::hash<std::basic_string_view<Elem>>>
hashed_basic_string_view<Elem> make_hashed_ref(
  std::basic_string_view<Elem> ref, size_t size,
  const Hasher& hasher = Hasher()) {
  return {hasher(ref), ref, size};
}

template<typename T>
inline size_t hash(const T* begin, size_t size) noexcept {
  assert(begin);

  size_t hash = 0;
  for (auto end = begin + size; begin != end;) {
    hash = hash_combine(hash, *begin++);
  }

  return hash;
}

using hashed_string_view = hashed_basic_string_view<char>;
using hashed_bytes_view = hashed_basic_string_view<byte_type>;

}  // namespace iresearch

// -----------------------------------------------------------------------------
// --SECTION--                                                 frozen extensions
// -----------------------------------------------------------------------------

namespace frozen {

template<>
struct elsa<std::string_view> {
  constexpr size_t operator()(std::string_view value) const noexcept {
    return elsa<frozen::string>{}({value.data(), value.size()});
  }
  constexpr std::size_t operator()(std::string_view value,
                                   std::size_t seed) const {
    return elsa<frozen::string>{}({value.data(), value.size()}, seed);
  }
};

}  // namespace frozen

// -----------------------------------------------------------------------------
// --SECTION--                                                   absl extensions
// -----------------------------------------------------------------------------

namespace absl {
namespace hash_internal {

template<typename Char>
struct HashImpl<::iresearch::hashed_basic_string_view<Char>> {
  size_t operator()(
    const ::iresearch::hashed_basic_string_view<Char>& value) const {
    return value.hash();
  }
};

}  // namespace hash_internal
}  // namespace absl

// -----------------------------------------------------------------------------
// --SECTION--                                                    std extensions
// -----------------------------------------------------------------------------

namespace std {

template<typename Char>
struct hash<::iresearch::hashed_basic_string_view<Char>> {
  size_t operator()(
    const ::iresearch::hashed_basic_string_view<Char>& value) const noexcept {
    return value.hash();
  }
};

}  // namespace std

#endif
