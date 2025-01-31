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

#pragma once

#include <filesystem>

#include "store/directory.hpp"
#include "store/directory_attributes.hpp"

namespace iresearch {

class fs_directory : public directory {
 public:
  static constexpr size_t kDefaultPoolSize = 8;

  explicit fs_directory(std::filesystem::path dir,
                        directory_attributes attrs = directory_attributes{},
                        size_t fd_pool_size = kDefaultPoolSize);

  using directory::attributes;
  directory_attributes& attributes() noexcept override { return attrs_; }

  index_output::ptr create(std::string_view name) noexcept override;

  const std::filesystem::path& directory() const noexcept;

  bool exists(bool& result, std::string_view name) const noexcept override;

  bool length(uint64_t& result, std::string_view name) const noexcept override;

  index_lock::ptr make_lock(std::string_view name) noexcept override;

  bool mtime(std::time_t& result,
             std::string_view name) const noexcept override;

  index_input::ptr open(std::string_view name,
                        IOAdvice advice) const noexcept override;

  bool remove(std::string_view name) noexcept override;

  bool rename(std::string_view src, std::string_view dst) noexcept override;

  bool sync(std::string_view name) noexcept override;

  bool visit(const visitor_f& visitor) const override;

 private:
  directory_attributes attrs_;
  std::filesystem::path dir_;
  size_t fd_pool_size_;
};

}  // namespace iresearch
