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

#include "so_utils.hpp"

#include "log.hpp"
#include "utils/file_utils.hpp"

#if defined(_MSC_VER)  // Microsoft compiler
#define NOMINMAX
#include <windows.h>

namespace {

std::string dlerror() {
  auto error = GetLastError();

  if (!error) {
    return std::string();
  }

  LPVOID lpMsgBuf;
  auto bufLen =
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR)&lpMsgBuf, 0, NULL);

  if (!bufLen) {
    return std::string();
  }

  auto lpMsgStr = (LPTSTR)lpMsgBuf;
  std::string result(lpMsgStr, lpMsgStr + bufLen);

  LocalFree(lpMsgBuf);

  return result;
}

}  // namespace

#elif defined(__GNUC__)  // GNU compiler
#include <dlfcn.h>
#else
#error define your compiler
#endif

namespace {

#if defined(_MSC_VER)  // Microsoft compiler
const std::wstring FILENAME_EXTENSION(L".dll");
#elif defined(__APPLE__)  // MacOS
const std::string FILENAME_EXTENSION(".dylib");
#elif defined(__GNUC__)   // GNU compiler
const std::string FILENAME_EXTENSION(".so");
#else
const std::string FILENAME_EXTENSION("");
#endif

}  // namespace

namespace iresearch {

void* load_library(const char* soname, int mode /* = 2 */) {
  if (!soname) {
    return nullptr;
  }

  std::filesystem::path name{soname};
  name += FILENAME_EXTENSION;

#if defined(_MSC_VER)  // Microsoft compiler
  UNUSED(mode);
  auto handle = static_cast<void*>(::LoadLibraryW(name.c_str()));
#elif defined(__GNUC__)  // GNU compiler
  auto handle = dlopen(name.c_str(), mode);
#endif

  if (!handle) {
#ifdef _WIN32
    IR_FRMT_ERROR("load failed of shared object: %s error: %s",
                  name.u8string().c_str(), dlerror().c_str());
#else
    IR_FRMT_ERROR("load failed of shared object: %s error: %s", name.c_str(),
                  dlerror());
#endif
  }

  return handle;
}

void* get_function(void* handle, const char* fname) {
#if defined(_MSC_VER)  // Microsoft compiler
  return static_cast<void*>(
    ::GetProcAddress(static_cast<HINSTANCE>(handle), fname));
#elif defined(__GNUC__)  // GNU compiler
  return dlsym(handle, fname);
#endif
}

bool free_library(void* handle) {
#if defined(_MSC_VER)  // Microsoft compiler
  return TRUE == ::FreeLibrary(static_cast<HINSTANCE>(handle));
#elif defined(__GNUC__)  // GNU compiler
  return dlclose(handle);
#endif
}

void load_libraries(std::string_view path, std::string_view prefix,
                    std::string_view suffix) {
  std::filesystem::path plugin_path{path};
  bool result;

  if (!file_utils::exists_directory(result, plugin_path.c_str()) || !result) {
    IR_FRMT_INFO("library load failed, not a plugin path: %s",
                 plugin_path.u8string().c_str());

    return;  // no plugins directory
  }

  auto visitor = [&plugin_path, &prefix, &suffix](auto name) -> bool {
    bool result;

    const auto path = plugin_path / name;

    if (!file_utils::exists_file(result, path.c_str())) {
      IR_FRMT_ERROR("Failed to identify plugin file: %s",
                    path.u8string().c_str());

      return false;
    }

    if (!result) {
      return true;  // skip non-files
    }

    const auto path_parts = irs::file_utils::path_parts(name);

    if (FILENAME_EXTENSION != path_parts.extension) {
      return true;  // skip non-library extensions
    }

    auto stem =
      std::filesystem::path{std::filesystem::path::string_type{path_parts.stem}}
        .string();

    if (stem.size() < prefix.size() + suffix.size() ||
        strncmp(stem.c_str(), prefix.data(), prefix.size()) != 0 ||
        strncmp(stem.c_str() + stem.size() - suffix.size(), suffix.data(),
                suffix.size()) != 0) {
      return true;  // filename does not match
    }

    const auto path_stem = plugin_path / stem;  // strip extension

    // FIXME TODO check double-load of same dll
    const void* handle = load_library(path_stem.string().c_str(), 1);

    if (!handle) {
      IR_FRMT_ERROR("library load failed for path: %s",
                    path_stem.string().c_str());
    }

    return true;
  };

  file_utils::visit_directory(plugin_path.c_str(), visitor, false);
}

}  // namespace iresearch
