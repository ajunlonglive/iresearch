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

#include "file_names.hpp"

#include <cinttypes>

#include "shared.hpp"

#if defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

namespace iresearch {

std::string file_name(uint64_t gen) {
  char buf[22] = "_";  // can hold : -2^63 .. 2^64-1, plus '_' prefix, plus 0
  return std::string(buf, 1 + std::sprintf(buf + 1, "%" PRIu64, gen));
}

std::string file_name(std::string_view prefix, uint64_t gen) {
  char buf[21];  // can hold : -2^63 .. 2^64-1, plus 0
  auto buf_size = sprintf(buf, "%" PRIu64, gen);

  std::string str;
  str.reserve(buf_size + prefix.size());
  str.append(prefix);
  str.append(buf, sprintf(buf, "%" PRIu64, gen));

  return str;
}

std::string file_name(std::string_view name, std::string_view ext) {
  std::string out;
  out.reserve(1 + name.size() + ext.size());
  out.append(name);
  out += '.';
  out.append(ext);

  return out;
}

void file_name(std::string& out, std::string_view name, std::string_view ext) {
  out.clear();
  out.reserve(1 + name.size() + ext.size());
  out.append(name);
  out += '.';
  out.append(ext);
}

std::string file_name(std::string_view name, uint64_t gen, std::string_view ext) {
  char buf[21];  // can hold : -2^63 .. 2^64-1, plus 0
  auto buf_size = sprintf(buf, "%" PRIu64, gen);

  std::string out;
  out.reserve(2 + name.size() + buf_size + ext.size());
  out.append(name);
  out += '.';
  out.append(buf, buf_size);
  out += '.';
  out.append(ext);

  return out;
}

}  // namespace iresearch

#if defined(_MSC_VER)
#pragma warning(default : 4996)
#endif
