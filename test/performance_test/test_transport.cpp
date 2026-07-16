// Measure performance of uTP socket operations on various transport backends: regular UDP socket vs Ouisync.

#include <asio_utp/socket.hpp>
#include <asio_utp/udp_multiplexer.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/program_options.hpp>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "../util/test_dir.h"
#include "../../util/random.h"
#include "../../util/wait_condition.h"
#include "logger.h"
#include "ouiservice/ouisync/socket.h"
#include "ouisync/service.hpp"

namespace po = boost::program_options;
namespace asio = boost::asio;
using namespace ouinet;
using boost::system::error_code;

// -------------------------------------------------------------------------------------------------
enum class Transport {
    udp,
    ouisync
};

std::ostream& operator << (std::ostream& os, Transport transport) {
    switch (transport) {
        case Transport::udp: return os << "udp";
        case Transport::ouisync: return os << "ouisync";
        default: throw std::runtime_error("invalid transport");
    }
}

std::istream& operator >> (std::istream& is, Transport& transport) {
    std::string token;
    is >> token;

    if (boost::iequals(token, "udp") || boost::iequals(token, "u")) {
        transport = Transport::udp;
        return is;
    }

    if (boost::iequals(token, "ouisync") || boost::iequals(token, "o")) {
        transport = Transport::ouisync;
        return is;
    }

    throw std::runtime_error(std::string("invalid transport: ") + token);
}

// -------------------------------------------------------------------------------------------------

size_t parse_bytes(const std::string& s) {
    std::istringstream is(s);

    double value = 0.0;
    std::string suffix;

    is >> value >> std::ws >> suffix;

    if (boost::iequals(suffix, "b")) {
        return std::round(value);
    }

    if (boost::iequals(suffix, "kb") || boost::iequals(suffix, "k")) {
        return std::round(value * 1024.0);
    }

    if (boost::iequals(suffix, "mb") || boost::iequals(suffix, "m")) {
        return std::round(value * 1024.0 * 1024.0);
    }

    if (boost::iequals(suffix, "gb") || boost::iequals(suffix, "g")) {
        return std::round(value * 1024.0 * 1024.0 * 1024.0);
    }

    throw std::runtime_error(std::string("suffix not supported: ") + suffix);
}

// -------------------------------------------------------------------------------------------------

std::tuple<ouisync::Service, ouisync::Session, asio_utp::udp_multiplexer>
create_ouisync_socket(TestDir& root, const std::string& name, Async yield) {
    auto dir = root.make_subdir(name);
    auto service = ouisync::Service(yield.get_executor());
    service.start(dir.path(), name.c_str(), yield).value();

    auto session = ouisync::Session::connect(yield.get_executor(), dir.path(), yield).value();
    session.bind_network({ "quic/127.0.0.1:0" }, yield).value();

    auto ouisync_socket = ouisync_service::OuisyncSocket::open(session, asio::ip::udp::v4(), yield).value();
    asio_utp::udp_multiplexer mux(yield.get_executor());
    mux.bind(std::make_unique<ouisync_service::OuisyncSocket>(std::move(ouisync_socket)));

    return { std::move(service), std::move(session), std::move(mux) };
}


int main(int argc, const char** argv)
{
    po::options_description desc("Ouinet uTP performance test");
    desc.add_options()
        (
            "help,h",
            "Show help"
        )
        (
            "size,s",
            po::value<std::string>()->default_value("1kB"),
            "Amount of data (in bytes) to transfer"
        )
        (
            "sender-transport,S",
            po::value<Transport>(),
            "Sender's transport (udp or ouisync)"
        )
        (
            "receiver-transport,R",
            po::value<Transport>(),
            "Receiver's transport (udp or ouisync)"
        );

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 1;
    }

    auto size = parse_bytes(vm["size"].as<std::string>());

    Transport send_transport;
    if (vm.count("sender-transport")) {
        send_transport = vm["sender-transport"].as<Transport>();
    } else {
        std::cerr << "Missing --sender-transport" << std::endl;
        return 1;
    }

    Transport recv_transport;
    if (vm.count("receiver-transport")) {
        recv_transport = vm["receiver-transport"].as<Transport>();
    } else {
        std::cerr << "Missing --receiver-transport" << std::endl;
        return 1;
    }

    asio::io_context ctx;
    asio::spawn(
        ctx,
        [&] (asio::yield_context y) {
            Async yield(y);

            ouisync::init_log();

            TestDir root(fs::temp_directory_path() / fs::unique_path());
            root.delete_on_exit(true);

            asio_utp::socket send_socket(yield.get_executor());
            std::optional<ouisync::Service> send_service;
            switch (send_transport) {
                case Transport::udp: {
                    error_code ec;
                    send_socket.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
                    assert(!ec);
                    break;
                }
                case Transport::ouisync: {
                    auto [service, session, mux] = create_ouisync_socket(root, "send", yield);
                    send_service = std::move(service);

                    error_code ec;
                    send_socket.bind(mux, ec);
                    assert(!ec);

                    break;
                }
            }

            asio_utp::socket recv_socket(yield.get_executor());
            std::optional<ouisync::Service> recv_service;
            switch (recv_transport) {
                case Transport::udp: {
                    error_code ec;
                    recv_socket.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
                    assert(!ec);
                    break;
                }
                case Transport::ouisync: {
                    auto [service, session, mux] = create_ouisync_socket(root, "recv", yield);
                    recv_service = std::move(service);

                    error_code ec;
                    recv_socket.bind(mux, ec);
                    assert(!ec);

                    break;
                }
            }

            std::cout
                << "Sending " << size << " bytes from "
                << send_transport << "://" << send_socket.local_endpoint()
                <<  " to "
                << recv_transport << "://" << recv_socket.local_endpoint()
                << "." << std::endl;

            auto data = util::random::bytes(size);

            WaitCondition recv_wc(yield.get_executor());

            // Receiver
            yield.spawn([&, lock = recv_wc.lock()] (Async yield) {
                std::vector<uint8_t> buffer;
                buffer.reserve(data.size());

                recv_socket.async_accept(yield).value();

                asio::async_read(
                    recv_socket,
                    asio::dynamic_buffer(buffer, data.size()),
                    yield
                ).value();
                std::cout << "Received       " << buffer.size() << "/" << data.size() << " bytes." << std::endl;

                if (buffer == data) {
                    std::cerr << "Data matches" << std::endl;
                } else {
                    std::cerr << "Data doesn't match" << std::endl;
                }
            });

            // Sender
            yield.spawn([&] (Async yield) {
                send_socket
                    .async_connect(recv_socket.local_endpoint(), yield)
                    .value();

                auto n = asio::async_write(send_socket, asio::buffer(data), yield).value();
                std::cout << "Sent           " << n << "/" << data.size() << " bytes." << std::endl;

                recv_wc.wait(yield).value();
            });

            recv_wc.wait(yield).value();
        },
        [] (std::exception_ptr e) {
            try {
                if (e) std::rethrow_exception(e);
            } catch (const std::exception& e) {
                std::cerr << "Exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception" << std::endl;
            }
        }
    );

    ctx.run();

    return 0;
}
