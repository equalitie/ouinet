#pragma once

#include <array>
#include <vector>
#include <string>
#include <boost/utility/string_view.hpp>

namespace ouinet { namespace util {

namespace sha1_detail {
    struct Sha1;

    size_t size_of_Sha1();
    Sha1* init(void*);
    void update(Sha1*, const void*, size_t);
    std::array<uint8_t, 20> close(Sha1*);

    inline void update(Sha1* digest, boost::string_view sv)
    {
        update(digest, sv.data(), sv.size());
    }

    inline void update(Sha1* digest, const char* c)
    {
        update(digest, boost::string_view(c));
    }

    inline void update(Sha1* digest, const std::string& data)
    {
        update(digest, data.data(), data.size());
    }

    inline void update(Sha1* digest, const std::vector<unsigned char>& data)
    {
        update(digest, data.data(), data.size());
    }

    template<size_t N>
    inline void update(Sha1* digest, const std::array<uint8_t, N>& data)
    {
        update(digest, data.data(), N);
    }

    inline
    std::array<uint8_t, 20> sha1(Sha1* digest)
    {
        return close(digest);
    }

    template<class Arg, class... Rest>
    inline
    std::array<uint8_t, 20> sha1(Sha1* digest, const Arg& arg, const Rest&... rest)
    {
        update(digest, arg);
        return sha1(digest, rest...);
    }

} // detail namespace

/*
 * Usage:
 *
 * array<uint8_t, 20> hash = sha1(string("hello world"));
 *
 * Or:
 *
 * array<uint8_t, 20> hash = sha1(boost::string_view("hello world"));
 *
 * Or:
 *
 * string s = "hello ";
 * string_view sv = "world";
 * array<uint8_t, 20> hash = sha1(s + sv.to_string());
 *
 * Or:
 *
 * string s = "hello ";
 * string_view sv = "world";
 * array<uint8_t, 20> hash = sha1(hello, world);
 *
 * Note that the last one is more efficient than the one before because it
 * does not allocate a new string as a result of applying the operator+
 */

template<class Arg, class... Rest>
inline
std::array<uint8_t, 20> sha1(const Arg& arg, const Rest&... rest)
{
    using namespace sha1_detail;
    uint8_t mem[size_of_Sha1()];
    Sha1* digest = init(mem);
    return sha1(digest, arg, rest...);
}

}} // namespaces
