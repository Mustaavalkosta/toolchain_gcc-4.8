// The  -*- C++ -*- type traits classes for internal use in libstdc++

// Copyright (C) 2000, 2001, 2002, 2003, 2004, 2005
// Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING.  If not, write to the Free
// Software Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307,
// USA.

// As a special exception, you may use this file as part of a free software
// library without restriction.  Specifically, if other files instantiate
// templates or use macros or inline functions from this file, or you compile
// this file and link it with other files to produce an executable, this
// file does not by itself cause the resulting executable to be covered by
// the GNU General Public License.  This exception does not however
// invalidate any other reasons why the executable file might be covered by
// the GNU General Public License.

// Written by Gabriel Dos Reis <dosreis@cmla.ens-cachan.fr>

/** @file cpp_type_traits.h
 *  This is an internal header file, included by other library headers.
 *  You should not attempt to use it directly.
 */

#ifndef _CPP_TYPE_TRAITS_H
#define _CPP_TYPE_TRAITS_H 1

#pragma GCC system_header

#include <bits/c++config.h>

//
// This file provides some compile-time information about various types.
// These representations were designed, on purpose, to be constant-expressions
// and not types as found in <bits/type_traits.h>.  In particular, they
// can be used in control structures and the optimizer hopefully will do
// the obvious thing.
//
// Why integral expressions, and not functions nor types?
// Firstly, these compile-time entities are used as template-arguments
// so function return values won't work:  We need compile-time entities.
// We're left with types and constant  integral expressions.
// Secondly, from the point of view of ease of use, type-based compile-time
// information is -not- *that* convenient.  On has to write lots of
// overloaded functions and to hope that the compiler will select the right
// one. As a net effect, the overall structure isn't very clear at first
// glance.
// Thirdly, partial ordering and overload resolution (of function templates)
// is highly costly in terms of compiler-resource.  It is a Good Thing to
// keep these resource consumption as least as possible.
//
// See valarray_array.h for a case use.
//
// -- Gaby (dosreis@cmla.ens-cachan.fr) 2000-03-06.
//
// Update 2005: types are also provided and <bits/type_traits.h> has been
// removed.
//

// NB: g++ can not compile these if declared within the class
// __is_pod itself.
namespace __gnu_internal
{
  typedef char __one;
  typedef char __two[2];

  template <typename _Tp>
  __one __test_type (int _Tp::*);
  template <typename _Tp>
  __two& __test_type (...);
} // namespace __gnu_internal

// Forward declaration hack, should really include this from somewhere.
namespace __gnu_cxx
{
  template<typename _Iterator, typename _Container>
    class __normal_iterator;
} // namespace __gnu_cxx

struct __true_type { };
struct __false_type { };

namespace std
{
  template<bool>
    struct __truth_type
    { typedef __false_type __type; };

  template<>
    struct __truth_type<true>
    { typedef __true_type __type; };

  template<class _Sp, class _Tp>
    struct __traitor
    {
      enum { _M_type = _Sp::_M_type || _Tp::_M_type };
      typedef typename __truth_type<_M_type>::__type __type;
    };

  // Compare for equality of types.
  template<typename, typename>
    struct __are_same
    {
      enum
	{
	  _M_type = 0
	};
    };

  template<typename _Tp>
    struct __are_same<_Tp, _Tp>
    {
      enum
	{
	  _M_type = 1
	};
    };

  // Define a nested type if some predicate holds.
  template<typename, bool>
    struct __enable_if
    { 
    };

  template<typename _Tp>
    struct __enable_if<_Tp, true>
    {
      typedef _Tp _M_type;
    };

  // Holds if the template-argument is a void type.
  template<typename _Tp>
    struct __is_void
    {
      enum { _M_type = 0 };
      typedef __false_type __type;
    };

  template<>
    struct __is_void<void>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  //
  // Integer types
  //
  template<typename _Tp>
    struct __is_integer
    {
      enum { _M_type = 0 };
      typedef __false_type __type;
    };

  // Thirteen specializations (yes there are eleven standard integer
  // types; 'long long' and 'unsigned long long' are supported as
  // extensions)
  template<>
    struct __is_integer<bool>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<char>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<signed char>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned char>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

# ifdef _GLIBCXX_USE_WCHAR_T
  template<>
    struct __is_integer<wchar_t>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };
# endif

  template<>
    struct __is_integer<short>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned short>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<int>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned int>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<long>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned long>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<long long>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_integer<unsigned long long>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  //
  // Floating point types
  //
  template<typename _Tp>
    struct __is_floating
    {
      enum { _M_type = 0 };
      typedef __false_type __type;
    };

  // three specializations (float, double and 'long double')
  template<>
    struct __is_floating<float>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_floating<double>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  template<>
    struct __is_floating<long double>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  //
  // Pointer types
  //
  template<typename _Tp>
    struct __is_pointer
    {
      enum { _M_type = 0 };
      typedef __false_type __type;
    };

  template<typename _Tp>
    struct __is_pointer<_Tp*>
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  //
  // Normal iterator type
  //
  template<typename _Tp>
    struct __is_normal_iterator
    {
      enum { _M_type = 0 };
      typedef __false_type __type;
    };

  template<typename _Iterator, typename _Container>
    struct __is_normal_iterator< __gnu_cxx::__normal_iterator<_Iterator,
							      _Container> >
    {
      enum { _M_type = 1 };
      typedef __true_type __type;
    };

  //
  // An arithmetic type is an integer type or a floating point type
  //
  template<typename _Tp>
    struct __is_arithmetic
    : public __traitor<__is_integer<_Tp>, __is_floating<_Tp> >
    { };

  //
  // A fundamental type is `void' or and arithmetic type
  //
  template<typename _Tp>
    struct __is_fundamental
    : public __traitor<__is_void<_Tp>, __is_arithmetic<_Tp> >
    { };

  //
  // A scalar type is an arithmetic type or a pointer type
  // 
  template<typename _Tp>
    struct __is_scalar
    : public __traitor<__is_arithmetic<_Tp>, __is_pointer<_Tp> >
    { };

  //
  // For the immediate use, the following is a good approximation
  //
  template<typename _Tp>
    struct __is_pod
    {
      enum
	{
	  _M_type = (sizeof(__gnu_internal::__test_type<_Tp>(0))
		     != sizeof(__gnu_internal::__one))
	};
    };

} // namespace std

#endif //_CPP_TYPE_TRAITS_H
