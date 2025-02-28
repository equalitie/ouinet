#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "service.h"
#include "tunneller_service.h"

#include "../../logger.h"
#include "../../logger.h"
#include "../../util/signal.h"

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

  TunnellerService::TunnellerService(std::shared_ptr<Service> service, AsioExecutor _exec)
        :
    _service(service),
    _tcp_acceptor(_exec),
    _socket(_exec)
  {
        start_listen_and_accept();
    }

  void TunnellerService::start_listen_and_accept() {
    boost::asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 8998);

    sys::error_code ec;

    /// announce that we started listening on i2p port
    LOG_DEBUG("I2P tunneller openning port..");

    _tcp_acceptor.open(endpoint.protocol(), ec);
    if (ec) {
      LOG_ERROR( "Error: " + ec.message());
      return;
    }

    _tcp_acceptor.set_option(asio::socket_base::reuse_address(true));

    _tcp_acceptor.bind(endpoint, ec);
    if (ec) {
      _tcp_acceptor.close();
      LOG_ERROR( "Error: " + ec.message());
      return;
    }

    _tcp_acceptor.listen(asio::socket_base::max_connections, ec);

    //while(true) {
      _tcp_acceptor.async_accept(_socket, [this](boost::system::error_code ec) {
        if (!ec) {
          read_socket_data();
        } else {
          LOG_ERROR( "Error: " + ec.message());
          _tcp_acceptor.close();
          return;
        }
      });
      //}

  }

  void TunnellerService::read_socket_data() {
    boost::asio::async_read_until(
        _socket,
        _read_buffer, '\n',
        boost::bind(&TunnellerService::get_new_target_and_start_client,
                    this, boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred)
                                  );
        
        }

  void TunnellerService::get_new_target_and_start_client(const boost::system::error_code& error, std::size_t bytes_transferred)
  {
    if (!error)
      {
            std::istream is(&_read_buffer);
            std::string i2p_target_id;
            std::getline(is, i2p_target_id);

            LOG_DEBUG("Received: I2P seeder " + i2p_target_id);
            //consider input as a destination address and start a client.
            auto i2p_client = _service->build_client(i2p_target_id);

            //wait for the tunnel to get ready
            boost::asio::spawn([&](asio::yield_context yield) {
              sys::error_code ec;

              i2p_client->start(yield[ec]);
              if (ec) {
                //faild to establish tunnel
                LOG_ERROR( "Error: " + ec.message());
              } else {

                Signal<void()> cancel = Signal<void()>();
                auto i2p_seeder_stream = i2p_client->connect(yield, cancel);

                std::string request = "GET / HTTP/1.1\r\nHost: example.org\r\n\r\n";

                LOG_DEBUG("Requested payload from : I2P seeder ");
                //Write the bhttp request in the tunnel. 
                asio::async_write(i2p_seeder_stream, boost::asio::buffer(request), [&](boost::system::error_code ec, std::size_t /*length*/) {
                  if (ec) {
                    LOG_ERROR( "Error: " + ec.message());
                  } else {
                    boost::beast::flat_buffer con_rbuf;
                    //yield[ec].tag("read_req").run([&] (auto y) {
                    beast::http::request<http::string_body> req;
                    boost::asio::async_read(i2p_seeder_stream, con_rbuf, yield[ec]);
                      //async_boost:beast::http::async_read(i2p_seeder_stream, con_rbuf, req, yield[ec]);
                      //});

                    if (ec) {
                      LOG_ERROR( "Error: " + ec.message());
                    } else {
                      LOG_DEBUG( "Finish reading " + std::to_string(con_rbuf.size()) + " Bytes");
                  
                    }
                  }
              });
              }
            });
      }
        else
        {          
          LOG_ERROR( "Error: " + error.message());

        }
      // Start another asynchronous read
    read_socket_data();
            
  }







