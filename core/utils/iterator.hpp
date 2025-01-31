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

#ifndef IRESEARCH_ITERATOR_H
#define IRESEARCH_ITERATOR_H

#include <boost/iterator/iterator_facade.hpp>
#include <cassert>
#include <memory>

#include "misc.hpp"
#include "noncopyable.hpp"
#include "std.hpp"

namespace iresearch {

template<typename T>
struct iterator {
  virtual ~iterator() = default;
  virtual T value() const = 0;
  virtual bool next() = 0;
};

template<typename Key, typename Value, typename Iterator, typename Base,
         typename Less = std::less<Key>>
class iterator_adaptor : public Base {
 public:
  typedef Iterator iterator_type;
  typedef Key key_type;
  typedef Value value_type;
  typedef const value_type& const_reference;

  iterator_adaptor(iterator_type begin, iterator_type end,
                   const Less& less = Less())
    : begin_{begin}, cur_{begin}, end_{end}, less_{less} {}

  const_reference value() const noexcept override { return *cur_; }

  bool seek(key_type key) noexcept override {
    begin_ = std::lower_bound(cur_, end_, key, less_);
    return next();
  }

  bool next() noexcept override {
    if (begin_ == end_) {
      cur_ = begin_;  // seal iterator
      return false;
    }

    cur_ = begin_++;
    return true;
  }

 private:
  iterator_type begin_;
  iterator_type cur_;
  iterator_type end_;
  IRS_NO_UNIQUE_ADDRESS Less less_;
};

namespace detail {

template<typename Ptr>
struct extract_element_type {
  typedef typename Ptr::element_type value_type;
  typedef typename Ptr::element_type& reference;
  typedef typename Ptr::element_type* pointer;
};

template<typename Ptr>
struct extract_element_type<const Ptr> {
  typedef const typename Ptr::element_type value_type;
  typedef const typename Ptr::element_type& reference;
  typedef const typename Ptr::element_type* pointer;
};

template<typename Ptr>
struct extract_element_type<Ptr*> {
  typedef Ptr value_type;
  typedef Ptr& reference;
  typedef Ptr* pointer;
};

}  // namespace detail

//////////////////////////////////////////////////////////////////////////////
/// @class const_ptr_iterator
/// @brief iterator adapter for containers with the smart pointers
//////////////////////////////////////////////////////////////////////////////
template<typename IteratorImpl>
class ptr_iterator
  : public ::boost::iterator_facade<
      ptr_iterator<IteratorImpl>,
      typename detail::extract_element_type<typename std::remove_reference<
        typename IteratorImpl::reference>::type>::value_type,
      ::boost::random_access_traversal_tag> {
 private:
  typedef detail::extract_element_type<
    typename std::remove_reference<typename IteratorImpl::reference>::type>
    element_type;

  typedef ::boost::iterator_facade<
    ptr_iterator<IteratorImpl>,
    typename detail::extract_element_type<typename std::remove_reference<
      typename IteratorImpl::reference>::type>::value_type,
    ::boost::random_access_traversal_tag>
    base;

  typedef typename base::iterator_facade_ iterator_facade;

  typedef typename element_type::value_type base_element_type;

  template<typename T>
  struct adjust_const
    : irstd::adjust_const<typename element_type::value_type, T> {};

 public:
  typedef typename iterator_facade::reference reference;
  typedef typename iterator_facade::difference_type difference_type;

  ptr_iterator(const IteratorImpl& it) : it_(it) {}

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns downcasted reference to the iterator's value
  //////////////////////////////////////////////////////////////////////////////
  template<typename T>
  typename adjust_const<T>::reference as() const {
    typedef
      typename std::enable_if_t<std::is_base_of_v<base_element_type, T>, T>
        type;

    return down_cast<type>(dereference());
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns downcasted pointer to the iterator's value
  ///        or nullptr if there is no available conversion
  //////////////////////////////////////////////////////////////////////////////
  template<typename T>
  typename adjust_const<T>::pointer safe_as() const {
    typedef
      typename std::enable_if<std::is_base_of<base_element_type, T>::value,
                              T>::type type;

    reference it = dereference();
    return dynamic_cast<typename adjust_const<type>::pointer>(&it);
  }

 private:
  friend class ::boost::iterator_core_access;

  reference dereference() const {
    assert(*it_);
    return **it_;
  }
  void advance(difference_type n) { it_ += n; }
  difference_type distance_to(const ptr_iterator& rhs) const {
    return rhs.it_ - it_;
  }
  void increment() { ++it_; }
  void decrement() { --it_; }
  bool equal(const ptr_iterator& rhs) const { return it_ == rhs.it_; }

  IteratorImpl it_;
};

}  // namespace iresearch

#endif
