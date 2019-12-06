#include <boost/asio/spawn.hpp>
#include <iostream>
#include <string>

#include "namespaces.h"
#include "ouiservice.h"
#include "ouiservice/tcp.h"

//#include "util/crypto.h"

using namespace std;
using namespace ouinet;

int main(int argc, const char* argv[])
{
//    util::crypto_init();

    asio::io_context ctx;

    OuiServiceServer server(ctx.get_executor());

    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string("127.0.0.1"), 10203);
    server.add(make_unique<ouiservice::TcpOuiServiceServer>(ctx.get_executor(), endpoint));

    asio::spawn(ctx, [&ctx, &server] (asio::yield_context yield) {
        sys::error_code ec;
        server.start_listen(yield[ec]);

        if (ec) {
            std::cerr << "Failed to setup ouiservice server: " << ec.message() << endl;
            return;
        }

        std::cout << "Listening" << endl;

        while (true) {
            GenericStream connection = server.accept(yield[ec]);
            if (ec) {
                break;
            }

            asio::spawn(ctx, [connection = std::move(connection)] (asio::yield_context yield) mutable {
                sys::error_code ec;
                std::string line;
                while (true) {
                    char c;
                    size_t read = connection.async_read_some(asio::mutable_buffers_1(&c, 1), yield[ec]);
                    if (ec || !read) {
                        return;
                    }

                    line += c;
                    if (c == '\n') {
                        std::cerr << line;
                        while (line.size()) {
                            size_t written = connection.async_write_some(asio::const_buffers_1(line.data(), line.size()), yield[ec]);
                            if (ec || !written) {
                                return;
                            }
                            line = line.substr(written);
                        }
                    }
                }
            });
        }
    });

    ctx.run();
    return 0;
}
