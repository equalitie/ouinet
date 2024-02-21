#ifndef RANDOM_ACCESS_HANDLE_ADAPTER_HPP
#define RANDOM_ACCESS_HANDLE_ADAPTER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio.hpp>
#include "boost/asio/windows/random_access_handle.hpp"
#include <boost/asio/detail/config.hpp>
#include "../../../src/util/executor.h"

using io_context = boost::asio::io_context;
using executor = ouinet::util::AsioExecutor;
using native_handle_t = HANDLE;
using error_code = boost::system::error_code;
namespace errc = boost::system::errc;

//namespace boost {
//namespace asio {
//namespace windows {

//template <typename Executor = boost::asio::any_io_executor>
class basic_random_access_handle_extended : public boost::asio::windows::random_access_handle {
public:
  basic_random_access_handle_extended(io_context &ctx)
      : boost::asio::windows::random_access_handle(ctx) {}

  basic_random_access_handle_extended(executor exe)
      : boost::asio::windows::random_access_handle(exe) {}

  template <typename MutableBufferSequence, typename ReadHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
                                void (boost::system::error_code, std::size_t))
  async_read_some(const MutableBufferSequence& buffers,
                  BOOST_ASIO_MOVE_ARG(ReadHandler) handler)
  {
      // If you get an error on the following line it means that your handler does
      // not meet the documented type requirements for a ReadHandler.
      BOOST_ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

      boost::system::error_code ec;
      auto offset = current_position(ec);
      boost::asio::async_completion<ReadHandler,
              void (boost::system::error_code, std::size_t)> init(handler);

      this->impl_.get_service().async_read_some_at(
              this->impl_.get_implementation(), offset,
              buffers, init.completion_handler,
              this->impl_.get_implementation_executor());

      return init.result.get();
  }

  template <typename ConstBufferSequence, typename WriteHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
                                void (boost::system::error_code, std::size_t))
  async_write_some(const ConstBufferSequence& buffers,
                   BOOST_ASIO_MOVE_ARG(WriteHandler) handler)
  {
      // If you get an error on the following line it means that your handler does
      // not meet the documented type requirements for a WriteHandler.
      BOOST_ASIO_WRITE_HANDLER_CHECK(WriteHandler, handler) type_check;

      boost::system::error_code ec;
      auto offset = end_position(ec);
      boost::asio::async_completion<WriteHandler,
              void (boost::system::error_code, std::size_t)> init(handler);

      this->impl_.get_service().async_write_some_at(
              this->impl_.get_implementation(), offset,
              buffers, init.completion_handler,
              this->impl_.get_implementation_executor());

      return init.result.get();
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
typedef basic_random_access_handle_extended random_access_handle_extended;

//} // namespace windows
//} // namespace asio
//} // namespace boost

#endif // RANDOM_ACCESS_HANDLE_ADAPTER_HPP
