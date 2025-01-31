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

#include "string.hpp"

#include <absl/hash/hash.h>

namespace iresearch::hash_utils {

size_t Hash(const char* value, size_t size) noexcept {
  return absl::Hash<std::string_view>{}({value, size});
}

size_t Hash(const byte_type* value, size_t size) noexcept {
  static_assert(sizeof(byte_type) == sizeof(char));
  return Hash(reinterpret_cast<const char*>(value), size);
}

}  // namespace iresearch::hash_utils
