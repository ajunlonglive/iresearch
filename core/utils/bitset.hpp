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

#ifndef IRESEARCH_BITSET_H
#define IRESEARCH_BITSET_H

#include <cstring>
#include <memory>

#include "bit_utils.hpp"
#include "math_utils.hpp"
#include "memory.hpp"
#include "noncopyable.hpp"
#include "shared.hpp"

namespace iresearch {

template<typename Alloc>
class dynamic_bitset {
 public:
  using index_t = size_t;
  using word_t = size_t;
  using allocator_type =
    typename std::allocator_traits<Alloc>::template rebind_alloc<word_t>;
  using word_ptr_deleter_t =
    memory::allocator_array_deallocator<allocator_type>;
  using word_ptr_t = std::unique_ptr<word_t[], word_ptr_deleter_t>;

  constexpr FORCE_INLINE static size_t bits_to_words(size_t bits) noexcept {
    return bits / bits_required<word_t>() +
           size_t(0 != (bits % bits_required<word_t>()));
  }

  // returns corresponding bit index within a word for the
  // specified offset in bits
  constexpr FORCE_INLINE static size_t bit(size_t i) noexcept {
    return i % bits_required<word_t>();
  }

  // returns corresponding word index specified offset in bits
  constexpr FORCE_INLINE static size_t word(size_t i) noexcept {
    return i / bits_required<word_t>();
  }

  // returns corresponding offset in bits for the specified word index
  constexpr FORCE_INLINE static size_t bit_offset(size_t i) noexcept {
    return i * bits_required<word_t>();
  }

  dynamic_bitset(const Alloc& alloc = Alloc())
    : alloc_{alloc},
      data_{static_cast<typename word_ptr_t::pointer>(
              nullptr),  // workaround for broken check in MSVC2015
            word_ptr_deleter_t{alloc_, 0}} {}

  explicit dynamic_bitset(size_t bits, const Alloc& alloc = Alloc())
    : dynamic_bitset{alloc} {
    reset(bits);
  }

  dynamic_bitset(dynamic_bitset&& rhs) noexcept(
    std::is_nothrow_move_constructible_v<allocator_type>)
    : alloc_{std::move(alloc_)},
      bits_{rhs.bits_},
      words_{rhs.words_},
      data_{std::move(rhs.data_)} {
    rhs.bits_ = 0;
    rhs.words_ = 0;
  }

  dynamic_bitset& operator=(dynamic_bitset&& rhs) noexcept(
    std::is_nothrow_move_assignable_v<allocator_type>) {
    if (this != &rhs) {
      alloc_ = std::move(rhs.alloc_);
      bits_ = rhs.bits_;
      words_ = rhs.words_;
      data_ = std::move(rhs.data_);
      rhs.bits_ = 0;
      rhs.words_ = 0;
    }

    return *this;
  }

  void reset(size_t bits) {
    const auto num_words = bits_to_words(bits);

    if (num_words > words_) {
      data_ = memory::allocate_unique<word_t[]>(alloc_, num_words,
                                                memory::allocate_only);
    }

    words_ = num_words;
    bits_ = bits;
    clear();
  }

  bool operator==(const dynamic_bitset& rhs) const noexcept {
    if (this->size() != rhs.size()) {
      return false;
    }

    return 0 == std::memcmp(this->begin(), rhs.begin(), this->size());
  }

  bool operator!=(const dynamic_bitset& rhs) const noexcept {
    return !(*this == rhs);
  }

  // number of bits in bitset
  size_t size() const noexcept { return bits_; }

  // capacity in bits
  size_t capacity() const noexcept { return bits_required<word_t>() * words_; }

  size_t words() const noexcept { return words_; }

  const word_t* data() const noexcept { return data_.get(); }

  const word_t* begin() const noexcept { return data(); }
  const word_t* end() const noexcept { return data() + words_; }

  word_t operator[](size_t i) const noexcept {
    assert(i < words_);
    return data_[i];
  }

  template<typename T>
  void memset(const T& value) noexcept {
    memset(&value, sizeof(value));
  }

  void memset(const void* src, size_t size) noexcept {
    std::memcpy(data_.get(), src, std::min(size, words() * sizeof(word_t)));
    sanitize();
  }

  void set(size_t i) noexcept { set_bit(data_[word(i)], bit(i)); }

  void unset(size_t i) noexcept { unset_bit(data_[word(i)], bit(i)); }

  void reset(size_t i, bool set) noexcept {
    set_bit(data_[word(i)], bit(i), set);
  }

  bool test(size_t i) const noexcept {
    return check_bit(data_[word(i)], bit(i));
  }

  bool any() const noexcept {
    return std::any_of(begin(), end(), [](word_t w) { return w != 0; });
  }

  bool none() const noexcept { return !any(); }

  bool all() const noexcept { return (count() == size()); }

  void clear() noexcept {
    if (data_) {
      // passing nullptr to `std::memset` is undefined behavior
      std::memset(data_.get(), 0, sizeof(word_t) * words_);
    }
  }

  // counts bits set
  word_t count() const noexcept { return math::popcount(begin(), end()); }

 private:
  void sanitize() noexcept {
    assert(bits_ <= capacity());
    auto last_word_bits = bits_ % bits_required<word_t>();

    if (!last_word_bits) {
      return;  // no words or last word has all bits set
    }

    const auto mask = ~(~word_t(0) << (bits_ % bits_required<word_t>()));

    data_[words_ - 1] &= mask;
  }

  IRS_NO_UNIQUE_ADDRESS allocator_type alloc_;
  size_t bits_{};    // number of bits in a bitset
  size_t words_{};   // number of words used for storing data
  word_ptr_t data_;  // words array
};

using bitset = dynamic_bitset<std::allocator<size_t>>;

}  // namespace iresearch

#endif
