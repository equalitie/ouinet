#pragma once

#include <iterator>

#include "../parse/number.h"
#include "yield.h"

#include "cxx/dns.h"

namespace ouinet::util
{
    using tcp = asio::ip::tcp;
    using TcpLookup = tcp::resolver::results_type;
    using Answers = std::vector<asio::ip::address>;

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

    inline
    TcpLookup resolve( const std::string& host
                     , const std::string& port
                     , bool do_doh
                     , Cancel& cancel
                     , YieldContext yield)
    {
        using TcpEndpoint = typename TcpLookup::endpoint_type;

        boost::string_view portsv(port);
        auto portn_o = parse::number<unsigned short>(portsv);
        if (!portn_o) return or_throw<TcpLookup>(yield, asio::error::invalid_argument);

        // Build and return lookup if `host` is already a network address.
        {
            sys::error_code e;
            auto addr = asio::ip::make_address(host, e);
            if (!e) return TcpLookup::create(TcpEndpoint{std::move(addr), *portn_o}, host, port);
        }

        sys::error_code ec;
        dns::Resolver resolver{do_doh};
        const auto answers46= yield[ec].tag("resolve host").run([&] (auto y) {
            return resolver.resolve(host, y[ec]);
        });
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<TcpLookup>(yield, ec);

        const AddrsAsEndpoints<Answers, TcpEndpoint> eps{answers46, *portn_o};
        return TcpLookup::create(eps.begin(), eps.end(), host, port);
    }

}
