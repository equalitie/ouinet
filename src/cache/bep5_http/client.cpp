#include "client.h"
#include "../../util/sha1.h"
#include "../../util/bytes.h"
#include "../../util/file_io.h"

using namespace std;
using namespace ouinet;
using namespace cache::bep5_http;

namespace fs = boost::filesystem;

struct Client::Impl {
    asio::io_service& ios;
    fs::path cache_dir;
    Cancel cancel;

    Impl( asio::io_service& ios
        , fs::path cache_dir)
        : ios(ios)
        , cache_dir(move(cache_dir))
    {}

    void load( const std::string& key
             , GenericStream& sink
             , Cancel cancel
             , Yield yield)
    {
        sys::error_code ec;
        auto path = path_from_key(key);
        auto file = util::file_io::open_readonly(ios, path, ec);

        if (ec) {
            return or_throw(yield, asio::error::not_found);
        }

        flush_from_to(file, sink, yield);
    }

    void store( const std::string& key
              , const http::response_header<>& rs_hdr
              , GenericStream& rs_body
              , Cancel cancel
              , asio::yield_context yield)
    {
        sys::error_code ec;

        auto path = path_from_key(key);
        auto file = open_or_create(path, ec);
        if (ec) return or_throw(yield, ec);

        http::response<http::empty_body> rs_hdr_msg{rs_hdr};
        http::response_serializer<http::empty_body> rs_hdr_s(rs_hdr_msg);

        http::async_write_header(file, rs_hdr_s, yield[ec]);

        if (ec) {
            try_remove(path);
            return or_throw(yield, ec);
        }

        flush_from_to(rs_body, file, yield[ec]);

        if (ec) {
            try_remove(path);
            return or_throw(yield, ec);
        }
    }

    void try_remove(const fs::path& path)
    {
        sys::error_code ec_ignored;
        fs::remove(path, ec_ignored);
    }

    fs::path path_from_key(const std::string& key)
    {
        auto hash = util::bytes::to_hex(util::sha1(key));
        return cache_dir/"data"/hash;
    }

    asio::posix::stream_descriptor open_or_create( const fs::path& path
                                                 , sys::error_code& ec)
    {
        auto file = util::file_io::open_or_create(ios, path, ec);

        if (ec) return file;

        util::file_io::truncate(file, 0, ec);

        return file;
    }

    template<class Source, class Sink>
    void flush_from_to(Source& source, Sink& sink, asio::yield_context yield)
    {
        sys::error_code ec;
        std::array<uint8_t, 1 << 14> data;

        for (;;) {
            size_t length = source.async_read_some(asio::buffer(data), yield[ec]);
            if (ec) break;

            asio::async_write(sink, asio::buffer(data, length), yield[ec]);
            if (ec) break;
        }

        if (ec == asio::error::eof) {
            ec = sys::error_code();
        }

        return or_throw(yield, ec);
    }

    void stop() {
        cancel();
    }
};

/* static */
std::unique_ptr<Client>
Client::build( asio::io_service& ios
             , fs::path cache_dir
             , asio::yield_context yield)
{
    using ClientPtr = unique_ptr<Client>;

    sys::error_code ec;

    fs::create_directories(cache_dir/"data", ec);

    if (ec) return or_throw<ClientPtr>(yield, ec);

    unique_ptr<Impl> impl(new Impl(ios, move(cache_dir)));
    return unique_ptr<Client>(new Client(move(impl)));
}

Client::Client(unique_ptr<Impl> impl)
    : _impl(move(impl))
{}

void Client::load( const std::string& key
                 , GenericStream& sink
                 , Cancel cancel
                 , Yield yield)
{
    _impl->load(key, sink, cancel, yield);
}

void Client::store( const std::string& key
                  , const http::response_header<>& rs_hdr
                  , GenericStream& rs_body
                  , Cancel cancel
                  , asio::yield_context yield)
{
    _impl->store(key, rs_hdr, rs_body, cancel, yield);
}

Client::~Client()
{
    _impl->stop();
}
