#pragma once

#include "../ouiservice.h"

namespace ouinet {

// Contains the connection to I2P, in the form of a i2poui::Service.
class I2pOuiService
{

};

class I2pOuiServiceServer : OuiServiceImplementationServer
{
	public:
	// Represents an endpoint that i2p can listen on, in the form of a private key (?)
	I2pOuiServiceServer(I2pOuiService& service, I2pPrivateKey private_key);
	
	void start_listen(asio::yield_context yield);
	void stop_listen(asio::yield_context yield);
	
	GenericConnection accept(asio::yield_context yield);
	void cancel_accept(asio::yield_context yield);
};

class I2pOuiServiceClient : OuiServiceImplementationClient
{
	public:
	I2pOuiServiceClient(I2pOuiService& service, std::string endpoint);
	
	GenericConnection connect(asio::yield_context yield);
	void cancel_connect(asio::yield_context yield);
};

} // ouinet namespace
