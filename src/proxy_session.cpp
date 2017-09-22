#include "proxy_session.h"
#include "client.h"
#include "fail.h"

using namespace ouinet;

namespace http = boost::beast::http;
using namespace std;
using string_view = boost::beast::string_view;

static
pair<string_view, string_view> split_host_port(const string_view& hp)
{
    auto pos = hp.find(':');

    if (pos == string::npos) {
        return make_pair(hp, "80");
    }

    return make_pair(hp.substr(0, pos), hp.substr(pos+1));
}

void ProxySession::do_read()
{
    // Read a request
    http::async_read(_socket, _buffer, _req,
        [self = shared_from_this()] (auto ec, auto) {
            self->on_read(ec);
        });
}

void ProxySession::on_read(boost::system::error_code ec)
{
    // This means they closed the connection
    if(ec == http::error::end_of_stream)
        return do_close();

    if(ec)
        return fail(ec, "read");

    auto client = make_shared<Client>(_socket.get_io_service());

    // Send the response
    handle_request(move(client), move(_req));
}

void ProxySession::handle_request(shared_ptr<Client> c, Request req)
{
    // Make sure we can handle the method
    if( req.method() != http::verb::get && req.method() != http::verb::head) {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "Unknown HTTP-method";
        res.prepare_payload();
        send_response(move(res));
        return;
    }


    auto host_port = split_host_port(req["host"]);

    // Forward the request
    c->run( host_port.first
          , host_port.second
          , req
          , [self = shared_from_this(), req](Error error, auto res) {
                return self->send_response(move(res));
            });
}

template<class Res>
void ProxySession::send_response(Res&& res)
{
    // The lifetime of the message has to extend
    // for the duration of the async operation so
    // we use a shared_ptr to manage it.
    auto sr = make_shared<Res>(move(res));

    http::async_write(
        _socket,
        *sr,
        [this, sr, self = shared_from_this()] (Error ec, size_t) {
            if(ec == http::error::end_of_stream) {
                // This means we should close the connection, usually because
                // the response indicated the "Connection: close" semantic.
                return do_close();
            }

            if(ec)
                return fail(ec, "write");

            // Read another request
            do_read();
        });
}

void ProxySession::do_close()
{
    // Send a TCP shutdown
    boost::system::error_code ec;
    _socket.shutdown(tcp::socket::shutdown_send, ec);

    // At this point the connection is closed gracefully
}
