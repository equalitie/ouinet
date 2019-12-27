#pragma once

#include <fstream>

namespace ouinet { namespace stream {

template<class Inner> class Debug
{
public:
    Debug(Inner&& inner_stream)
        : _ex(inner_stream.get_executor())
        , _inner(new Inner(std::move(inner_stream)))
    {}

    Debug(const Debug&) = delete;
    Debug& operator=(const Debug&) = delete;

    Debug(Debug&&) = default;
    Debug& operator=(Debug&&) = default;

    void set_tag(std::string tag)
    {
        _tag = std::move(tag);
    }

    void set_log_file(const boost::filesystem::path& path) {
        _log_stream = std::fstream(path.native(), std::fstream::trunc);
    }

    template< class MutableBufferSequence
            , class Token>
    auto async_read_some(const MutableBufferSequence& bs, Token&& token)
    {
        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        using Sig = void(system::error_code, size_t);

        boost::asio::async_completion<Token, Sig> init(token);

        using Handler = std::decay_t<decltype(init.completion_handler)>;

        // XXX: Handler is non-copyable, but can we do this without allocation?
        auto handler = make_shared<Handler>(std::move(init.completion_handler));

        if (_inner) {
            _inner->async_read_some(
                    bs,
                    [this, h = move(handler), bs, tag = _tag]
                    (const system::error_code& ec, size_t size) {
                        if (tag.size()) {
                            if (ec) {
                                stream() << tag << " recv ec:" << ec.message() << "\n";
                            } else {
                                stream() << tag << " recv " << bufs_to_str(bs, size) << "\n";
                            }
                        }
                        (*h)(ec, size);
                    });
        }
        else {
            asio::post(_ex, [h = move(handler)] { (*h)(asio::error::bad_descriptor, 0); });
        }

        return init.result.get();
    }

    template< class ConstBufferSequence
            , class Token>
    auto async_write_some(const ConstBufferSequence& bs, Token&& token)
    {
        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        using Sig = void(system::error_code, size_t);

        boost::asio::async_completion<Token, Sig> init(token);

        using Handler = std::decay_t<decltype(init.completion_handler)>;

        // XXX: Handler is non-copyable, but can we do this without allocation?
        auto handler = make_shared<Handler>(std::move(init.completion_handler));

        if (_inner) {
            _inner->async_write_some(
                    bs,
                    [this, h = move(handler), bs, tag = _tag]
                    (const system::error_code& ec, size_t size) {
                        if (tag.size()) {
                            if (ec) {
                                stream() << tag << " sent ec:" << ec.message() << "\n";
                            } else {
                                stream() << tag << " sent " << bufs_to_str(bs, size) << "\n";
                            }
                        }
                        (*h)(ec, size);
                    });
        }
        else {
            asio::post(_ex, [h = move(handler)] { (*h)(asio::error::bad_descriptor, 0); });
        }

        return init.result.get();
    }

    void close() { return _inner->close(); }

    asio::executor get_executor() { return _ex; }

    bool is_open() const {
        if (!_inner) return false;
        return _inner->is_open();
    }

private:
    std::ostream& stream() {
        if (_log_stream) return *_log_stream;
        return std::cerr;
    }

private:
    static void write_readable(std::stringstream& s, const char c)
    {
        const char* hex = "0123456789abcdef";

        if (c >= ' ' && c <= '~') {
            s << c;
        } else {
            s << "\\\\x" << hex[(c >> 4) & 0xf] << hex[c & 0xf];
        }
    }

    template<class Bufs>
    static
    std::string bufs_to_str(const Bufs& bs, size_t size) {
        std::stringstream ss;
        for (auto b : bs) {
            const char* c = (const char*) b.data();
            for (size_t i = 0; i < b.size() && size; i++) {
                write_readable(ss, c[i]);
                --size;
            }
        }
        return ss.str(); 
    }

private:
    asio::executor _ex;
    std::shared_ptr<Inner> _inner;
    std::string _tag;
    boost::optional<std::fstream> _log_stream;
};

}} // namespaces
