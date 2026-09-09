#define BOOST_TEST_MODULE async_datagram_buffer
#include <boost/test/unit_test.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/network_v4.hpp>

#include "ouiservice/ouisync/buffer.h"
#include "util/async.h"
#include "util/async_test.h"
#include "util/condition_variable.h"
#include "util/random.h"
#include "util/unwrap.h"

using namespace ouinet;
namespace asio = boost::asio;

struct Datagram {
    asio::ip::udp::endpoint ep;
    std::string data;

    bool operator == (const Datagram&) const = default;
    bool operator != (const Datagram&) const = default;
};

std::ostream& operator << (std::ostream& os, const Datagram& d) {
    return os << "(" << d.ep << ", " << d.data << ")";
}

BOOST_AUTO_TEST_CASE(sanity) {
    async_test([] (Async yield) {
        size_t count = 1000;

        std::vector<Datagram> sent;
        std::vector<Datagram> received;

        ouisync_service::detail::AsyncDatagramBuffer buffer(yield.get_executor(), 32 * 1024);

        ConditionVariable cv(yield.get_executor());
        bool send_done = false;
        bool recv_done = false;

        // sender
        yield.spawn([&] (Async yield) {
            for (int i = 0; i < count; ++i) {
                asio::ip::udp::endpoint ep(
                    asio::ip::address_v4(util::random::number<uint32_t>()),
                    util::random::number<uint16_t>()
                );

                size_t size = util::random::number<size_t>(32, 512);
                auto data = util::random::printable_ascii(size);

                unwrap(buffer.async_push(asio::buffer(data), ep, yield));

                sent.push_back({ ep, std::move(data) });
            }

            buffer.close();

            send_done = true;
            cv.notify();
        });

        // receiver
        yield.spawn([&] (Async yield) {
            while (true) {
                std::string data;
                asio::ip::udp::endpoint ep;

                data.resize(buffer.peek());

                auto n = buffer.async_pop(asio::buffer(data), ep, yield);
                if (!n) {
                    break;
                }

                received.push_back({ ep, std::move(data) });
            }

            recv_done = true;
            cv.notify();
        });

        while (!send_done || !recv_done) {
            unwrap(cv.wait(yield));
        }

        BOOST_REQUIRE_EQUAL_COLLECTIONS(sent.begin(), sent.end(), received.begin(), received.end());
    });
}
