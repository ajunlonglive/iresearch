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

#include "utils/register.hpp"

#include "compression.hpp"

// list of statically loaded scorers via init()
#ifndef IRESEARCH_DLL
  #include "lz4compression.hpp"
  #include "delta_compression.hpp"
#endif

namespace {

struct value{
  explicit value(
      irs::compression::compressor_factory_f compressor_factory = nullptr,
      irs::compression::decompressor_factory_f decompressor_factory = nullptr)
    : compressor_factory_(compressor_factory),
      decompressor_factory_(decompressor_factory) {
  }

  bool empty() const noexcept {
    return !compressor_factory_ || !decompressor_factory_;
  }

  bool operator==(const value& other) const noexcept {
    return compressor_factory_ == other.compressor_factory_ &&
           decompressor_factory_ == other.decompressor_factory_;
  }

  bool operator!=(const value& other) const noexcept {
    return !(*this == other);
  }

  const irs::compression::compressor_factory_f compressor_factory_;
  const irs::compression::decompressor_factory_f decompressor_factory_;
};

constexpr std::string_view kFileNamePrefix("libcompression-");

class compression_register
    : public irs::tagged_generic_register<irs::string_ref, value,
                                          irs::string_ref, compression_register> {
 protected:
  virtual std::string key_to_filename(const key_type& key) const override {
    std::string filename(kFileNamePrefix.size() + key.size(), 0);

    std::memcpy(
      filename.data(),
      kFileNamePrefix.data(),
      kFileNamePrefix.size());

    std::memcpy(
      filename.data() + kFileNamePrefix.size(),
      key.c_str(),
      key.size());

    return filename;
  }
};

struct identity_compressor final : irs::compression::compressor {
  virtual irs::bytes_ref compress(
      irs::byte_type* in,
      size_t size,
      irs::bstring& /*buf*/) override {
    return { in, size };
  }

  virtual void flush(irs::data_output& /*out*/) override { }
}; // identity_compressor

identity_compressor kIdentityCompressor;

}

namespace iresearch {
namespace compression {

compression_registrar::compression_registrar(
    const type_info& type,
    compressor_factory_f compressor_factory,
    decompressor_factory_f decompressor_factory,
    const char* source /*= nullptr*/) {
  string_ref const source_ref(source);
  const auto new_entry = ::value(compressor_factory, decompressor_factory);

  auto entry = compression_register::instance().set(
    type.name(),
    new_entry,
    source_ref.null() ? nullptr : &source_ref);

  registered_ = entry.second;

  if (!registered_ && new_entry != entry.first) {
    auto* registered_source = compression_register::instance().tag(type.name());

    if (source && registered_source) {
      IR_FRMT_WARN(
        "type name collision detected while registering compression, ignoring: type '%s' from %s, previously from %s",
        type.name().c_str(),
        source,
        registered_source->c_str()
      );
    } else if (source) {
      IR_FRMT_WARN(
        "type name collision detected while registering compression, ignoring: type '%s' from %s",
        type.name().c_str(),
        source
      );
    } else if (registered_source) {
      IR_FRMT_WARN(
        "type name collision detected while registering compression, ignoring: type '%s', previously from %s",
        type.name().c_str(),
        registered_source->c_str()
      );
    } else {
      IR_FRMT_WARN(
        "type name collision detected while registering compression, ignoring: type '%s'",
        type.name().c_str()
      );
    }
  }
}

bool exists(string_ref name, bool load_library /*= true*/ ) {
  return !compression_register::instance().get(name, load_library).empty();
}

compressor::ptr get_compressor(
    string_ref name,
    const options& opts,
    bool load_library /*= true*/) noexcept {
  try {
    auto* factory = compression_register::instance().get(name, load_library).compressor_factory_;

    return factory ? factory(opts) : nullptr;
  } catch (...) {
    IR_FRMT_ERROR("Caught exception while getting an analyzer instance"); // cppcheck-suppress syntaxError
  }

  return nullptr;
}

decompressor::ptr get_decompressor(string_ref name, bool load_library /*= true*/) noexcept {
  try {
    auto* factory = compression_register::instance().get(name, load_library).decompressor_factory_;

    return factory ? factory() : nullptr;
  } catch (...) {
    IR_FRMT_ERROR("Caught exception while getting an analyzer instance");
  }

  return nullptr;
}

void init() {
#ifndef IRESEARCH_DLL
  lz4::init();
  delta::init();
  none::init();
#endif
}

void load_all(std::string_view path) {
  load_libraries(path, kFileNamePrefix, "");
}

bool visit(const std::function<bool(string_ref)>& visitor) {
  compression_register::visitor_t wrapper = [&visitor](string_ref key)->bool {
    return visitor(key);
  };

  return compression_register::instance().visit(wrapper);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                raw implementation
// -----------------------------------------------------------------------------

/*static*/ void none::init() {
#ifndef IRESEARCH_DLL
  // match registration below
  REGISTER_COMPRESSION(none, &none::compressor, &none::decompressor);
#endif
}

REGISTER_COMPRESSION(none, &none::compressor, &none::decompressor);

compressor::ptr compressor::identity() noexcept {
  return memory::to_managed<compressor, false>(&kIdentityCompressor);
}

} // compression
}
