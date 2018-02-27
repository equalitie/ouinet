#pragma once

#include "../ouiservice.h"

// Contains the connection to I2P, in the form of a i2poui::Service.
class I2pOuiService
{

};

class I2pOuiServiceServer : OuiServiceImplementationServer
{
	public:
	// Represents an endpoint that i2p can listen on, in the form of a private key (?)
	I2pOuiServiceServer(I2pOuiService& service, I2pPrivateKey private_key);
	
	sys::error_code start_listen(asio::yield_context yield);
	void stop_listen(asio::yield_context yield);
	
	Result<std::unique_ptr<GenericConnection>> accept(asio::yield_context yield);
	void cancel_accept(asio::yield_context yield);
};

class I2pOuiServiceClient : OuiServiceImplementationClient
{
	public:
	I2pOuiServiceClient(I2pOuiService& service);
	
	bool match_endpoint(std::string endpoint);
	Result<std::unique_ptr<GenericConnection>> connect(asio::yield_context yield);
	void cancel_connect(asio::yield_context yield);
};
