#include "util.h"

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>


using namespace std;


// Chaining the following two with something like
// <https://github.com/sneumann/mzR/blob/master/src/boost_aux/boost/iostreams/filter/base64.hpp>
// would be way cooler.`:)`

// Based on <https://stackoverflow.com/a/27559848> by user "Zangetsu".
template <class Filter>
string zlib_filter(const string& in) {
    stringstream in_ss;
    in_ss << in;

    boost::iostreams::filtering_streambuf<boost::iostreams::input> zip;
    zip.push(Filter());
    zip.push(in_ss);

    stringstream out_ss;
    boost::iostreams::copy(zip, out_ss);
    return out_ss.str();
}

string ouinet::util::zlib_compress(const string& in) {
    return zlib_filter<boost::iostreams::zlib_compressor>(in);
}

// TODO: Catch and report decompression errors.
string ouinet::util::zlib_decompress(const string& in, sys::error_code& ec) {
    return zlib_filter<boost::iostreams::zlib_decompressor>(in);
}

// Based on <https://stackoverflow.com/a/28471421> by user "ltc".
string ouinet::util::base64_encode(const boost::string_view in) {
    using namespace boost::archive::iterators;
    using It = base64_from_binary<transform_width<string::const_iterator, 6, 8>>;
    string out(It(in.begin()), It(in.end()));  // encode to base64
    return out.append((3 - in.size() % 3) % 3, '=');  // add padding
}

string ouinet::util::base64_decode(const boost::string_view in) {
    using namespace boost::archive::iterators;
    using It = transform_width<binary_from_base64<string::const_iterator>, 8, 6>;
    return boost::algorithm::trim_right_copy_if(string(It(begin(in)), It(end(in))), [](char c) {
        return c == '\0';
    });
}
