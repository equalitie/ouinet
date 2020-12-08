#pragma once

#include <boost/system/system_error.hpp>

// https://www.boost.org/doc/libs/1_71_0/libs/outcome/doc/html/motivation/plug_error_code2.html

namespace ouinet { namespace cache {

enum class MultiPeerReaderErrc {
    inconsistent_hash = 1,
    expected_head,
    expected_first_chunk_hdr,
    expected_chunk_body,
    block_is_too_big,
    expected_chunk_hdr,
    no_peers,
    expected_trailer_or_end_of_response,
    trailer_received_twice,
    expected_no_more_data
};

boost::system::error_code make_error_code(MultiPeerReaderErrc);

}} // namespaces

namespace boost { namespace system {
    template<> struct is_error_code_enum<ouinet::cache::MultiPeerReaderErrc>
    {
        BOOST_STATIC_CONSTANT(bool, value = true);
    };
}} // namespaces
