////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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

#ifndef IRESEARCH_ATTRIBUTES_PROVIDER_H
#define IRESEARCH_ATTRIBUTES_PROVIDER_H

#include "type_id.hpp"

NS_ROOT

struct attribute;

//////////////////////////////////////////////////////////////////////////////
/// @class attribute_provider
/// @brief base class for all objects with externally visible attributes
//////////////////////////////////////////////////////////////////////////////
struct IRESEARCH_API attribute_provider {
  virtual const attribute* get(type_info::type_id type) const = 0;
};

//////////////////////////////////////////////////////////////////////////////
/// @brief convenient helper for getting attributes of a specific type
//////////////////////////////////////////////////////////////////////////////
template<typename T,
         typename Provider,
         typename = std::enable_if_t<std::is_base_of_v<attribute, T>>>
inline const T* get(const Provider& attrs) noexcept {
  return static_cast<const T*>(attrs.get(type<T>::id()));
}

NS_END

#endif
