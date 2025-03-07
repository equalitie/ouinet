#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include "client.h"

namespace ouinet {
namespace ouiservice {
namespace i2poui {

/**
 *  An Experimental service which listen on a tcp socket for i2p destination
 *  and make a tunnel. Each tunnel is managed by a separate  ouinet i2p client
*/
class TunnellerService : public std::enable_shared_from_this<Service> {
public:
  TunnellerService(std::shared_ptr<Service> service, AsioExecutor _exec);

private:
  void start_listen_and_accept();

  void read_socket_data();
  void get_new_target_and_start_client(const boost::system::error_code& error, std::size_t bytes_transferred);

  
  std::shared_ptr<Service> _service;
  AsioExecutor _exec;

  boost::asio::ip::tcp::acceptor _tcp_acceptor;
  boost::asio::ip::tcp::socket _socket;
  boost::asio::streambuf _read_buffer;

  std::unique_ptr<Client> _i2p_client;

};

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace


