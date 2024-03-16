#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/windows/random_access_handle.hpp>
#include <windows.h>

using namespace boost::asio;
using namespace boost::system;
using error_code = boost::system::error_code;
using native_handle_t = HANDLE;

class random_access_handle_extended : public boost::asio::windows::random_access_handle {
public:
    explicit random_access_handle_extended(const executor_type &ex) : basic_random_access_handle(ex) {}

    template <typename MutableBufferSequence, typename ReadHandler>
    void async_read_some(const MutableBufferSequence& buffer, ReadHandler handler) {
        boost::system::error_code ec;
        auto offset = current_position(ec);
        this->async_read_some_at(offset, buffer, handler);
    }

    template <typename ConstBufferSequence, typename WriteHandler>
    void async_write_some(const ConstBufferSequence& buffer, WriteHandler handler) {
        boost::system::error_code ec;
        auto offset = end_position(ec);
        this->async_write_some_at(offset, buffer, handler);
    }

    static
    error_code last_error()
    {
        return make_error_code(static_cast<errc::errc_t>(errno));
    }

    size_t
    current_position(error_code& ec)
    {
        native_handle_t native_handle = this->native_handle();
        auto offset = SetFilePointer(native_handle, 0, NULL, FILE_CURRENT);
        if(INVALID_SET_FILE_POINTER ==  offset)
        {
            ec = last_error();
            if (!ec) ec = make_error_code(errc::no_message);
            return size_t(-1);
        }

        return offset;
    }

    size_t
    end_position(error_code& ec)
    {
        native_handle_t native_handle = this->native_handle();
        auto offset = SetFilePointer(native_handle, 0, NULL, FILE_END);
        if(INVALID_SET_FILE_POINTER ==  offset)
        {
            ec = last_error();
            if (!ec) ec = make_error_code(errc::no_message);
            return size_t(-1);
        }

        return offset;
    }

};
