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

    if (argc < 2) {
        std::cerr << "Usage: ouiservice-client <message>\n";
        return 1;
    }
    std::string message(argv[1]);
    message += "\n";

    asio::io_context ctx;

    OuiServiceClient client(ctx.get_executor());

    auto endpoint = Endpoint{Endpoint::TcpEndpoint, "127.0.0.1:10203"};
    client.add(endpoint, make_unique<ouiservice::TcpOuiServiceClient>(ctx.get_executor(), endpoint.endpoint_string));

    asio::spawn(ctx, [&client, &message] (asio::yield_context yield) {
        sys::error_code ec;
        client.start(yield[ec]);

        if (ec) {
            std::cerr << "Failed to setup ouiservice client: " << ec.message() << endl;
            return;
        }

        Signal<void()> cancel;
        auto connection = client.connect(yield[ec], cancel).connection;
        if (ec) {
            std::cerr << "Failed to connect to server: " << ec.message() << endl;
            return;
        }

        while (message.size()) {
            size_t written = connection.async_write_some(asio::const_buffers_1(message.data(), message.size()), yield[ec]);
            if (ec || !written) {
                return;
            }
            message = message.substr(written);
        }

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
                break;
            }
        }
    });

    ctx.run();
    return 0;
}
