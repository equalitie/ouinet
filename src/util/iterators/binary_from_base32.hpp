#ifndef BOOST_ARCHIVE_ITERATORS_BINARY_FROM_BASE32_HPP
#define BOOST_ARCHIVE_ITERATORS_BINARY_FROM_BASE32_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// binary_from_base32.hpp

// Adapted from Robert Ramsey's "binary_from_base64.hpp".

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <boost/assert.hpp>

#include <boost/serialization/throw_exception.hpp>
#include <boost/static_assert.hpp>

#include <boost/iterator/transform_iterator.hpp>
#include <boost/archive/iterators/dataflow_exception.hpp>

namespace boost { 
namespace archive {
namespace iterators {

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// convert base32 characters to binary data

namespace detail {

template<class CharType>
struct to_5_bit {
    typedef CharType result_type;
    CharType operator()(CharType t) const{
        static const signed char lookup_table[] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,26,27,28,29,30,31,-1,-1,-1,-1,-1, 0,-1,-1, // render '=' as 0
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1
        };
        // metrowerks trips this assertion - how come?
        #if ! defined(__MWERKS__)
        BOOST_STATIC_ASSERT(128 == sizeof(lookup_table));
        #endif
        signed char value = -1;
        if((unsigned)t <= 127)
            value = lookup_table[(unsigned)t];
        if(-1 == value)
            boost::serialization::throw_exception(
                // TODO: A specific exception should be added here for Base32.
                //dataflow_exception(dataflow_exception::invalid_base32_character)
                dataflow_exception(dataflow_exception::invalid_conversion)
            );
        return value;
    }
};

} // namespace detail

// note: what we would like to do is
// template<class Base, class CharType = typename Base::value_type>
//  typedef transform_iterator<
//      from_5_bit<CharType>,
//      transform_width<Base, 5, sizeof(Base::value_type) * 8, CharType>
//  > base32_from_binary;
// but C++ won't accept this.  Rather than using a "type generator" and
// using a different syntax, make a derivation which should be equivalent.
//
// Another issue addressed here is that the transform_iterator doesn't have
// a templated constructor.  This makes it incompatible with the dataflow
// ideal.  This is also addressed here.

template<
    class Base, 
    class CharType = typename boost::iterator_value<Base>::type
>
class binary_from_base32 : public
    transform_iterator<
        detail::to_5_bit<CharType>,
        Base
    >
{
    friend class boost::iterator_core_access;
    typedef transform_iterator<
        detail::to_5_bit<CharType>,
        Base
    > super_t;
public:
    // make composible buy using templated constructor
    template<class T>
    binary_from_base32(T  start) :
        super_t(
            Base(static_cast< T >(start)),
            detail::to_5_bit<CharType>()
        )
    {}
    // intel 7.1 doesn't like default copy constructor
    binary_from_base32(const binary_from_base32 & rhs) : 
        super_t(
            Base(rhs.base_reference()),
            detail::to_5_bit<CharType>()
        )
    {}
//    binary_from_base32(){};
};

} // namespace iterators
} // namespace archive
} // namespace boost

#endif // BOOST_ARCHIVE_ITERATORS_BINARY_FROM_BASE32_HPP
