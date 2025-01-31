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

#pragma once

#include <memory>
#include <vector>

#include "shared.hpp"
#include "std.hpp"
#include "string.hpp"

namespace iresearch {

////////////////////////////////////////////////////////////////////////////////
/// @brief compile-time type identifier
////////////////////////////////////////////////////////////////////////////////
template<typename T>
constexpr std::string_view ctti() noexcept {
  return {IRESEARCH_CURRENT_FUNCTION};
}

////////////////////////////////////////////////////////////////////////////////
/// @brief SFINAE
////////////////////////////////////////////////////////////////////////////////
#define DEFINE_HAS_MEMBER(member)                                            \
  template<typename T>                                                       \
  class has_member_##member {                                                \
   private:                                                                  \
    using yes_type = char;                                                   \
    using no_type = long;                                                    \
    using type = std::remove_reference_t<std::remove_cv_t<T>>;               \
    template<typename U>                                                     \
    static yes_type test(decltype(&U::member));                              \
    template<typename U>                                                     \
    static no_type test(...);                                                \
                                                                             \
   public:                                                                   \
    static constexpr bool value = sizeof(test<type>(0)) == sizeof(yes_type); \
  };                                                                         \
  template<typename T>                                                       \
  inline constexpr auto has_member_##member##_v =                            \
    has_member_##member<T>::value

#define HAS_MEMBER(type, member) has_member_##member##_v<type>

// ----------------------------------------------------------------------------
// --SECTION--                                             template type traits
// ----------------------------------------------------------------------------

template<typename... Types>
struct template_traits_t;

template<typename First, typename... Second>
struct template_traits_t<First, Second...> {
  static constexpr size_t count() noexcept {
    return 1 + template_traits_t<Second...>::count();
  }

  static constexpr size_t size() noexcept {
    return sizeof(First) + template_traits_t<Second...>::size();
  }

  static constexpr size_t size_aligned(size_t start = 0) noexcept {
    return template_traits_t<Second...>::size_aligned(
      template_traits_t<First>::size_max_aligned(start));
  }

  static constexpr size_t align_max(size_t max = 0) noexcept {
    return template_traits_t<Second...>::align_max(
      std::max(max, std::alignment_of<First>::value));
  }

  static constexpr size_t size_max(size_t max = 0) noexcept {
    return template_traits_t<Second...>::size_max(std::max(max, sizeof(First)));
  }

  static constexpr size_t offset_aligned(size_t start = 0) noexcept {
    typedef std::alignment_of<First> align_t;

    return start +
           ((align_t::value - (start % align_t::value)) %
            align_t::value)  // padding
           + sizeof(First);
  }

  static constexpr size_t size_max_aligned(size_t start = 0,
                                           size_t max = 0) noexcept {
    return template_traits_t<Second...>::size_max_aligned(
      start, (std::max)(max, offset_aligned(start)));
  }

  template<typename T>
  static constexpr bool is_convertible() noexcept {
    return std::is_convertible<First, T>::value ||
           template_traits_t<Second...>::template is_convertible<T>();
  }

  template<typename T>
  static constexpr bool in_list() noexcept {
    return std::is_same<First, T>::value ||
           template_traits_t<Second...>::template in_list<T>();
  }
};  // template_traits_t

template<>
struct template_traits_t<> {
  static constexpr size_t count() noexcept { return 0; }

  static constexpr size_t size() noexcept { return 0; }

  static constexpr size_t size_aligned(size_t start = 0) noexcept {
    return start;
  }

  static constexpr size_t align_max(size_t max = 0) noexcept { return max; }

  static constexpr size_t size_max(size_t max = 0) noexcept { return max; }

  static constexpr size_t offset_aligned(size_t start = 0) noexcept {
    return start;
  }

  static constexpr size_t size_max_aligned(size_t start = 0,
                                           size_t max = 0) noexcept {
    return max ? max : start;
  }

  template<typename T>
  static constexpr bool is_convertible() noexcept {
    return false;
  }

  template<typename T>
  static constexpr bool in_list() noexcept {
    return false;
  }
};  // template_traits_t

///////////////////////////////////////////////////////////////////////////////
/// @returns true if type 'T' is convertible to one of the specified 'Types',
///          false otherwise
///////////////////////////////////////////////////////////////////////////////
template<typename T, typename... Types>
constexpr bool is_convertible() noexcept {
  return template_traits_t<Types...>::template is_convertible<T>();
}

///////////////////////////////////////////////////////////////////////////////
/// @returns true if type 'T' is present in the specified list of 'Types',
///          false otherwise
///////////////////////////////////////////////////////////////////////////////
template<typename T, typename... Types>
constexpr bool in_list() noexcept {
  return template_traits_t<Types...>::template in_list<T>();
}

///////////////////////////////////////////////////////////////////////////////
/// @returns if 'T' is 'std::shared_ptr<T>' provides the member constant value
///          equal to 'true', or 'false' otherwise
///////////////////////////////////////////////////////////////////////////////
template<typename T>
struct is_shared_ptr : std::false_type {};

template<typename T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};

template<typename T>
inline constexpr bool is_shared_ptr_v = is_shared_ptr<T>::value;

///////////////////////////////////////////////////////////////////////////////
/// @returns if 'T' is 'std::unique_ptr<T, D>' provides the member constant
/// value
///          equal to 'true', or 'false' otherwise
///////////////////////////////////////////////////////////////////////////////
template<typename T>
struct is_unique_ptr : std::false_type {};

template<typename T, typename D>
struct is_unique_ptr<std::unique_ptr<T, D>> : std::true_type {};

template<typename T, typename D>
struct is_unique_ptr<std::unique_ptr<T[], D>> : std::true_type {};

template<typename T>
inline constexpr bool is_unique_ptr_v = is_unique_ptr<T>::value;

///////////////////////////////////////////////////////////////////////////////
/// @returns if 'T' is 'std::vector<T, A>' provides the member constant
/// value equal to 'true', or 'false' otherwise
///////////////////////////////////////////////////////////////////////////////
template<typename T>
struct is_vector : std::false_type {};

template<typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type {};

template<typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

}  // namespace iresearch
