#include <boost/asio/spawn.hpp>
#include <string>

#include "namespaces.h"
#include "task.h"
#include "ouiservice.h"
#include "ouiservice/tcp.h"

using namespace ouinet;

int main(int argc, const char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: ouiservice-client <message>\n";
        return 1;
    }
    std::string message(argv[1]);
    message += "\n";

    asio::io_context ctx;

    OuiServiceClient client(ctx.get_executor());

    auto endpoint = asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 10203);
    client.add(endpoint, make_unique<ouiservice::TcpOuiServiceClient>(ctx.get_executor(), endpoint));

    task::spawn_detached(ctx, [&client, &message] (asio::yield_context yield) {
        sys::error_code ec;
        client.start(yield[ec]);

        if (ec) {
            std::cerr << "Failed to setup ouiservice client: " << ec.message() << "\n";
            return;
        }

        Signal<void()> cancel;
        auto connection = client.connect(yield[ec], cancel).connection;
        if (ec) {
            std::cerr << "Failed to connect to server: " << ec.message() << "\n";
            return;
        }

        while (message.size()) {
            size_t written = connection.async_write_some(asio::const_buffer(message.data(), message.size()), yield[ec]);
            if (ec || !written) {
                return;
            }
            message = message.substr(written);
        }

        std::string line;
        while (true) {
            char c;
            size_t read = connection.async_read_some(asio::mutable_buffer(&c, 1), yield[ec]);
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
