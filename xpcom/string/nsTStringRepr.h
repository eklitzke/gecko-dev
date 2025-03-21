/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsTStringRepr_h
#define nsTStringRepr_h

#include <type_traits> // std::enable_if

#include "mozilla/Char16.h"
#include "mozilla/fallible.h"
#include "nsStringFlags.h"
#include "nsCharTraits.h"

template <typename T> class nsTSubstringTuple;
template <typename T> class nsTLiteralString;

// The base for string comparators
template <typename T> class nsTStringComparator
{
public:
  typedef T char_type;

  nsTStringComparator() {}

  virtual int operator()(const char_type*, const char_type*,
                         uint32_t, uint32_t) const = 0;
};

// The default string comparator (case-sensitive comparision)
template <typename T> class nsTDefaultStringComparator
  : public nsTStringComparator<T>
{
public:
  typedef T char_type;

  nsTDefaultStringComparator() {}

  virtual int operator()(const char_type*, const char_type*,
                         uint32_t, uint32_t) const override;
};

extern template class nsTDefaultStringComparator<char>;
extern template class nsTDefaultStringComparator<char16_t>;

namespace mozilla {

// This is mainly intended to be used in the context of nsTStrings where
// we want to enable a specific function only for a given character class. In
// order for this technique to work the member function needs to be templated
// on something other than `T`. We keep this in the `mozilla` namespace rather
// than `nsTStringRepr` as it's intentionally not dependent on `T`.
//
// The 'T' at the end of `Char[16]OnlyT` is refering to the `::type` portion
// which will only be defined if the character class is correct. This is similar
// to `std::enable_if_t` which is available in C++14, but not C++11.
//
// `CharType` is generally going to be a shadowed type of `T`.
//
// Example usage of a function that will only be defined if `T` == `char`:
//
// template <typename T>
// class nsTSubstring : public nsTStringRepr<T> {
//   template <typename Q = T, typename EnableForChar = typename CharOnlyT<Q>>
//   int Foo() { return 42; }
// };
//
// Please note that we had to use a separate type `Q` for this to work. You
// will get a semi-decent compiler error if you use `T` directly.

template <typename CharType> using CharOnlyT =
  typename std::enable_if<std::is_same<char, CharType>::value>::type;

template <typename CharType> using Char16OnlyT =
  typename std::enable_if<std::is_same<char16_t, CharType>::value>::type;

namespace detail {

// nsTStringRepr defines a string's memory layout and some accessor methods.
// This class exists so that nsTLiteralString can avoid inheriting
// nsTSubstring's destructor. All methods on this class must be const because
// literal strings are not writable.
//
// This class is an implementation detail and should not be instantiated
// directly, nor used in any way outside of the string code itself. It is
// buried in a namespace to discourage its use in function parameters.
// If you need to take a parameter, use [const] ns[C]Substring&.
// If you need to instantiate a string, use ns[C]String or descendents.
//
// NAMES:
//   nsStringRepr for wide characters
//   nsCStringRepr for narrow characters
template <typename T> class nsTStringRepr
{
public:
  typedef mozilla::fallible_t fallible_t;

  typedef T char_type;

  typedef nsCharTraits<char_type> char_traits;
  typedef typename char_traits::incompatible_char_type incompatible_char_type;

  typedef nsTStringRepr<T> self_type;
  typedef self_type base_string_type;

  typedef nsTSubstring<T> substring_type;
  typedef nsTSubstringTuple<T> substring_tuple_type;
  typedef nsTLiteralString<T> literalstring_type;

  typedef nsReadingIterator<char_type> const_iterator;
  typedef nsWritingIterator<char_type> iterator;

  typedef nsTStringComparator<char_type> comparator_type;

  typedef char_type* char_iterator;
  typedef const char_type* const_char_iterator;

  typedef uint32_t index_type;
  typedef uint32_t size_type;

  // These are only for internal use within the string classes:
  typedef StringDataFlags DataFlags;
  typedef StringClassFlags ClassFlags;

  // Reading iterators.
  const_char_iterator BeginReading() const
  {
    return mData;
  }
  const_char_iterator EndReading() const
  {
    return mData + mLength;
  }

  // Deprecated reading iterators.
  const_iterator& BeginReading(const_iterator& aIter) const
  {
    aIter.mStart = mData;
    aIter.mEnd = mData + mLength;
    aIter.mPosition = aIter.mStart;
    return aIter;
  }

  const_iterator& EndReading(const_iterator& aIter) const
  {
    aIter.mStart = mData;
    aIter.mEnd = mData + mLength;
    aIter.mPosition = aIter.mEnd;
    return aIter;
  }

  const_char_iterator& BeginReading(const_char_iterator& aIter) const
  {
    return aIter = mData;
  }

  const_char_iterator& EndReading(const_char_iterator& aIter) const
  {
    return aIter = mData + mLength;
  }

  // Accessors.
  template <typename U, typename Dummy> struct raw_type { typedef const U* type; };
#if defined(MOZ_USE_CHAR16_WRAPPER)
  template <typename Dummy> struct raw_type<char16_t, Dummy> { typedef char16ptr_t type; };
#endif

  // Returns pointer to string data (not necessarily null-terminated)
  const typename raw_type<T, int>::type Data() const
  {
    return mData;
  }

  size_type Length() const
  {
    return mLength;
  }

  DataFlags GetDataFlags() const
  {
    return mDataFlags;
  }

  bool IsEmpty() const
  {
    return mLength == 0;
  }

  bool IsLiteral() const
  {
    return !!(mDataFlags & DataFlags::LITERAL);
  }

  bool IsVoid() const
  {
    return !!(mDataFlags & DataFlags::VOIDED);
  }

  bool IsTerminated() const
  {
    return !!(mDataFlags & DataFlags::TERMINATED);
  }

  char_type CharAt(index_type aIndex) const
  {
    NS_ASSERTION(aIndex < mLength, "index exceeds allowable range");
    return mData[aIndex];
  }

  char_type operator[](index_type aIndex) const
  {
    return CharAt(aIndex);
  }

  char_type First() const;

  char_type Last() const;

  size_type NS_FASTCALL CountChar(char_type) const;
  int32_t NS_FASTCALL FindChar(char_type, index_type aOffset = 0) const;

  inline bool Contains(char_type aChar) const
  {
    return FindChar(aChar) != kNotFound;
  }

  // Equality.
  bool NS_FASTCALL Equals(const self_type&) const;
  bool NS_FASTCALL Equals(const self_type&, const comparator_type&) const;

  bool NS_FASTCALL Equals(const substring_tuple_type& aTuple) const;
  bool NS_FASTCALL Equals(const substring_tuple_type& aTuple,
                          const comparator_type& aComp) const;

  bool NS_FASTCALL Equals(const char_type* aData) const;
  bool NS_FASTCALL Equals(const char_type* aData,
                          const comparator_type& aComp) const;

#if defined(MOZ_USE_CHAR16_WRAPPER)
  template <typename Q = T, typename EnableIfChar16 = Char16OnlyT<Q>>
  bool NS_FASTCALL Equals(char16ptr_t aData) const
  {
    return Equals(static_cast<const char16_t*>(aData));
  }
  template <typename Q = T, typename EnableIfChar16 = Char16OnlyT<Q>>
  bool NS_FASTCALL Equals(char16ptr_t aData, const comparator_type& aComp) const
  {
    return Equals(static_cast<const char16_t*>(aData), aComp);
  }
#endif

  // An efficient comparison with ASCII that can be used even
  // for wide strings. Call this version when you know the
  // length of 'data'.
  bool NS_FASTCALL EqualsASCII(const char* aData, size_type aLen) const;
  // An efficient comparison with ASCII that can be used even
  // for wide strings. Call this version when 'data' is
  // null-terminated.
  bool NS_FASTCALL EqualsASCII(const char* aData) const;

  // EqualsLiteral must ONLY be applied to an actual literal string, or
  // a char array *constant* declared without an explicit size.
  // Do not attempt to use it with a regular char* pointer, or with a
  // non-constant char array variable. Use EqualsASCII for them.
  // The template trick to acquire the array length at compile time without
  // using a macro is due to Corey Kosak, with much thanks.
  template<int N>
  inline bool EqualsLiteral(const char (&aStr)[N]) const
  {
    return EqualsASCII(aStr, N - 1);
  }

  // The LowerCaseEquals methods compare the ASCII-lowercase version of
  // this string (lowercasing only ASCII uppercase characters) to some
  // ASCII/Literal string. The ASCII string is *not* lowercased for
  // you. If you compare to an ASCII or literal string that contains an
  // uppercase character, it is guaranteed to return false. We will
  // throw assertions too.
  bool NS_FASTCALL LowerCaseEqualsASCII(const char* aData,
                                        size_type aLen) const;
  bool NS_FASTCALL LowerCaseEqualsASCII(const char* aData) const;

  // LowerCaseEqualsLiteral must ONLY be applied to an actual
  // literal string, or a char array *constant* declared without an
  // explicit size.  Do not attempt to use it with a regular char*
  // pointer, or with a non-constant char array variable. Use
  // LowerCaseEqualsASCII for them.
  template<int N>
  bool LowerCaseEqualsLiteral(const char (&aStr)[N]) const
  {
    return LowerCaseEqualsASCII(aStr, N - 1);
  }

  // Returns true if this string overlaps with the given string fragment.
  bool IsDependentOn(const char_type* aStart, const char_type* aEnd) const
  {
     // If it _isn't_ the case that one fragment starts after the other ends,
     // or ends before the other starts, then, they conflict:
     //
     //   !(f2.begin >= f1.aEnd || f2.aEnd <= f1.begin)
     //
     // Simplified, that gives us:
    return (aStart < (mData + mLength) && aEnd > mData);
  }

protected:
  nsTStringRepr() = delete; // Never instantiate directly

  constexpr
  nsTStringRepr(char_type* aData, size_type aLength,
               DataFlags aDataFlags, ClassFlags aClassFlags)
    : mData(aData)
    , mLength(aLength)
    , mDataFlags(aDataFlags)
    , mClassFlags(aClassFlags)
  {
  }

  char_type* mData;
  size_type mLength;
  DataFlags mDataFlags;
  ClassFlags const mClassFlags;
};

extern template class nsTStringRepr<char>;
extern template class nsTStringRepr<char16_t>;

} // namespace detail
} // namespace mozilla

template <typename T>
int NS_FASTCALL
Compare(const mozilla::detail::nsTStringRepr<T>& aLhs,
        const mozilla::detail::nsTStringRepr<T>& aRhs,
        const nsTStringComparator<T>& = nsTDefaultStringComparator<T>());

template <typename T>
inline bool
operator!=(const mozilla::detail::nsTStringRepr<T>& aLhs,
           const mozilla::detail::nsTStringRepr<T>& aRhs)
{
  return !aLhs.Equals(aRhs);
}

template <typename T>
inline bool
operator!=(const mozilla::detail::nsTStringRepr<T>& aLhs,
           const T* aRhs)
{
  return !aLhs.Equals(aRhs);
}

template <typename T>
inline bool
operator<(const mozilla::detail::nsTStringRepr<T>& aLhs,
          const mozilla::detail::nsTStringRepr<T>& aRhs)
{
  return Compare(aLhs, aRhs) < 0;
}

template <typename T>
inline bool
operator<=(const mozilla::detail::nsTStringRepr<T>& aLhs,
           const mozilla::detail::nsTStringRepr<T>& aRhs)
{
  return Compare(aLhs, aRhs) <= 0;
}

template <typename T>
inline bool
operator==(const mozilla::detail::nsTStringRepr<T>& aLhs,
           const mozilla::detail::nsTStringRepr<T>& aRhs)
{
  return aLhs.Equals(aRhs);
}

template <typename T>
inline bool
operator==(const mozilla::detail::nsTStringRepr<T>& aLhs,
           const T* aRhs)
{
  return aLhs.Equals(aRhs);
}

template <typename T>
inline bool
operator>=(const mozilla::detail::nsTStringRepr<T>& aLhs,
           const mozilla::detail::nsTStringRepr<T>& aRhs)
{
  return Compare(aLhs, aRhs) >= 0;
}

template <typename T>
inline bool
operator>(const mozilla::detail::nsTStringRepr<T>& aLhs,
          const mozilla::detail::nsTStringRepr<T>& aRhs)
{
  return Compare(aLhs, aRhs) > 0;
}

#endif
