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
      LOG_DEBUG("I2P tunneller listening...");

      _tcp_acceptor.async_accept(_socket, [this](boost::system::error_code ec) {
          if (!ec) {
            read_socket_data();
          } else {
            LOG_ERROR( "Error: " + ec.message());
            _tcp_acceptor.close();
            return;
          }
      });
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
             _i2p_client = _service->build_client(i2p_target_id);

            //wait for the tunnel to get ready
            boost::asio::spawn([&](asio::yield_context yield) {
              sys::error_code ec;

              _i2p_client->start(yield[ec]);

              if (ec) {
                //faild to establish tunnel
                LOG_ERROR( "Error in starting i2p tunnel: " + ec.message());
              } else {

                  for(int retry = 0; retry < 10; retry++) {
                  LOG_DEBUG("try number " + std::to_string(retry));

                  Cancel cancel;
                  auto i2p_seeder_stream = _i2p_client->connect(yield, cancel);

                  LOG_DEBUG("connecting to I2P seeder done");

                  std::string request = "GET http://httpforver.com/ HTTP/1.1\r\nHost: httpforever.com\r\n\r\n";

                  //std::string request = "GET http://192.168.31.136:7080/ HTTP/1.1\r\nHost: 192.168.31.136:7080\r\n\r\n";
                  //std::string request = "GET http://23.215.0.132/ HTTP/1.1\r\nHost: 23.215.0.132\r\n\r\n";
                  //std::string request = "GET http://example.org/ HTTP/1.1\r\nHost: example.org\r\n\r\n";
                  //std::string request = "GET http://127.0.0.1:7080/ HTTP/1.1\r\nHost: 127.0.0.1:7080\r\n\r\n";
                  //std::string request = "GET / HTTP/1.1\r\nHost: 127.0.0.1:7080\r\n\r\n";
                  //std::string request = "GET / HTTP/1.1\nHost: localhost:7080\n";
                  // beast::http::request<http::string_body> req{beast::http::verb::get, "/why", 11};
                  // req.set(beast::http::field::host, "127.0.0.1:7080");
                  // req.prepare_payload();

                  // std::string raw_request = req.method_string().to_string() + " " + req.target().to_string() + " HTTP/" + std::to_string(req.version() / 10) + "." + std::to_string(req.version() % 10) + "\r\n";

                  // for (const auto& field : req) {
                  //   raw_request += string(beast::http::to_string(field.name())) + ": " + field.value().to_string() + "\r\n";
                  // }

                  // raw_request += "\r\n";
                  // raw_request += req.body();

                  LOG_DEBUG("Requesting payload directly: " + request);

                  boost::asio::io_context io_context;

                  boost::asio::ip::tcp::resolver resolver(io_context);
                  boost::asio::ip::tcp::resolver::results_type endpoints = resolver.resolve("127.0.0.1", "7080");

                  boost::asio::ip::tcp::socket socket(io_context);
                  boost::asio::connect(socket, endpoints);

                  boost::asio::write(socket, boost::asio::buffer(request));

                  LOG_DEBUG("Requesting payload from : I2P seeder " + request);
                  //Write the http request in the tunnel.

                  asio::async_write(i2p_seeder_stream, boost::asio::buffer(request), yield[ec]);

                  if (ec) {
                    LOG_ERROR( "Error in writing the request: " + ec.message());
                  } else {
                    LOG_DEBUG("Requested payload from : I2P seeder ");

                    //yield[ec].tag("read_req").run([&] (auto y) {
                    //boost::asio::async_read(_i2p_seeder_stream, con_rbuf, [&](boost::system::error_code ec, std::size_t /*length*/) {
                    boost::beast::flat_buffer con_rbuf;
                    beast::http::response<http::string_body> response;
                  async_boost:beast::http::async_read(i2p_seeder_stream, con_rbuf, response, yield[ec]);
                    LOG_DEBUG("Response from I2P seeder received");
                  
                    if (ec) {
                      LOG_ERROR( "Error in getting i2p seeder response: " + ec.message());
                    } else {
                      LOG_DEBUG( "Finish reading " + std::to_string(con_rbuf.size()) + " Bytes");
                      break;
                    }                  
                  }
                  }//for
                  
              }
                           
            });
      }
    else
      {          
          LOG_ERROR( "Error in getting new i2p seeder: " + error.message());

      }

      // Start another asynchronous read
    read_socket_data();
            
  }







