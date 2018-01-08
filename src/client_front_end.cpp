#include "client_front_end.h"
#include "generic_connection.h"
#include <ipfs_cache/client.h>


using namespace std;
using namespace ouinet;

using Request = http::request<http::string_body>;
using string_view = beast::string_view;

static void redirect_back( GenericConnection& con
                         , const Request& req
                         , asio::yield_context yield)
{
    http::response<http::string_body> res{http::status::ok, req.version()};

    auto body =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "    <head>\n"
        "        <meta http-equiv=\"refresh\" content=\"0; url=http://localhost\"/>\n"
        "    </head>\n"
        "</html>\n";

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(false);
    res.body() = body;
    res.prepare_payload();

    sys::error_code ec;
    http::async_write(con, res, yield[ec]);
}

struct ToggleInput {
    string_view text;
    string_view name;
    bool current_value;
};

ostream& operator<<(ostream& os, const ToggleInput& i) {
    auto cur_value  = i.current_value ? "enabled" : "disabled";
    auto next_value = i.current_value ? "disable" : "enable";

    return os <<
          "<form method=\"get\">\n"
          "    " << i.text << ": " << cur_value << "&nbsp;"
                    "<input type=\"submit\" "
                           "name=\""  << i.name << "\" "
                           "value=\"" << next_value << "\"/>\n"
          "</form>\n";
}

void ClientFrontEnd::serve( GenericConnection& con
                          , const Endpoint& injector_ep
                          , const Request& req
                          , std::shared_ptr<ipfs_cache::Client>& cache_client
                          , asio::yield_context yield)
{
    http::response<http::string_body> res{http::status::ok, req.version()};

    auto target = req.target();

    if (target.find('?') != string::npos) {
        // XXX: Extra primitive value parsing.
        if (target.find("?injector_proxy=enable") != string::npos) {
            _injector_proxying_enabled = true;
        }
        else if (target.find("?injector_proxy=disable") != string::npos) {
            _injector_proxying_enabled = false;
        }
        else if (target.find("?auto_refresh=enable") != string::npos) {
            _auto_refresh_enabled = true;
        }
        else if (target.find("?auto_refresh=disable") != string::npos) {
            _auto_refresh_enabled = false;
        }
        else if (target.find("?ipfs_cache=enable") != string::npos) {
            _ipfs_cache_enabled = true;
        }
        else if (target.find("?ipfs_cache=disable") != string::npos) {
            _ipfs_cache_enabled = false;
        }
        redirect_back(con, req, yield);
        return;
    }

    stringstream ss;
    ss << "<!DOCTYPE html>\n"
          "<html>\n";
    if (_auto_refresh_enabled) {
          ss << "    <head>\n"
                "        <meta http-equiv=\"refresh\" content=\"1\"/>\n"
                "    </head>\n";
    }
    ss << "    <body>\n";

    ss << ToggleInput{"Auto refresh",   "auto_refresh",   _auto_refresh_enabled};
    ss << ToggleInput{"Injector proxy", "injector_proxy", _injector_proxying_enabled};
    ss << ToggleInput{"IPFS Cache",     "ipfs_cache",     _ipfs_cache_enabled};

    ss << "<br>\n";
    ss << "Injector endpoint: " << injector_ep << "<br>\n";

    if (cache_client) {
        ss << "        <h2>Database</h2>\n";
        ss << "        IPNS: " << cache_client->ipns() << "<br>\n";
        ss << "        IPFS: " << cache_client->ipfs() << "<br>\n";
        ss << "        <pre>\n";
        ss << cache_client->json_db().dump(4);
        ss << "        </pre>\n";
    }

    ss << "    </body>\n"
          "</html>\n";

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(false);
    res.body() = ss.str();
    res.prepare_payload();

    sys::error_code ec;
    http::async_write(con, res, yield[ec]);
}

