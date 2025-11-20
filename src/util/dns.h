#pragma once

#include <iterator>

namespace ouinet::util
{
    // Transforms addresses to endpoints with the given port.
    template <class Addrs, class Endpoint>
    class AddrsAsEndpoints
    {
    public:
        using value_type = Endpoint;
        using addrs_iterator = typename Addrs::const_iterator;

        AddrsAsEndpoints(const Addrs& addrs, unsigned short port)
            : _addrs(addrs), _port(port)
        {
        }

        class const_iterator
        {
        public:
            // Iterator requirements
            using iterator_category = std::input_iterator_tag;
            using value_type = Endpoint;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type*;
            using reference = value_type&;

            const_iterator(const addrs_iterator& it, unsigned short port)
                : _it(it), _port(port)
            {
            }

            value_type operator*() const { return {*_it, _port}; }

            const_iterator& operator++()
            {
                ++_it;
                return *this;
            }

            bool operator==(const const_iterator& other) const { return _it == other._it; }
            bool operator!=(const const_iterator& other) const { return _it != other._it; }

        private:
            addrs_iterator _it;
            unsigned short _port;
        };

        const_iterator begin() const { return {_addrs.begin(), _port}; };
        const_iterator end() const { return {_addrs.end(), _port}; };

    private:
        const Addrs& _addrs;
        unsigned short _port;
    };
}
