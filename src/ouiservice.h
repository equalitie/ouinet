#pragma once

#include <memory>
#include <boost/asio/spawn.hpp>

#include "generic_connection.h"
#include "result.h"

namespace ouinet {

class OuiServiceImplementationServer
{
	public:
	virtual ~OuiServiceImplementationServer() {}
	
	virtual sys::error_code start_listen(asio::yield_context yield) = 0;
	virtual void stop_listen(asio::yield_context yield) = 0;
	
	virtual Result<std::unique_ptr<GenericConnection>> accept(asio::yield_context yield) = 0;
	virtual void cancel_accept(asio::yield_context yield) = 0;
};

class OuiServiceServer
{
	public:
	OuiServiceServer(asio::io_service& ios);
	
	void add(OuiServiceImplementationServer* implementation);
	
	sys::error_code start_listen(asio::yield_context yield);
	void stop_listen(asio::yield_context yield);
	
	Result<std::unique_ptr<GenericConnection>> accept(asio::yield_context yield);
	void cancel_accept(asio::yield_context yield);
	
	private:
	asio::io_service& _ios;
	std::vector<OuiServiceImplementationServer*> _implementations;
	bool _is_listening;
	std::vector<OuiServiceImplementationServer*> _listening_implementations;
};

class OuiServiceImplementationClient
{
	public:
	virtual ~OuiServiceImplementationClient() {}
	
	virtual bool match_endpoint(std::string endpoint) = 0;
	virtual Result<std::unique_ptr<GenericConnection>> connect(asio::yield_context yield) = 0;
	virtual void cancel_connect(asio::yield_context yield) = 0;
};

class OuiServiceClient
{
	public:
	OuiServiceClient(asio::io_service& ios);
	
	void add(OuiServiceImplementationClient* implementation);
	
	void Result<std::unique_ptr<GenericConnection>>(asio::yield_context yield) = 0;
	void cancel_connect(asio::yield_context yield) = 0;
	
	private:
	asio::io_service& _ios;
	std::vector<OuiServiceImplementationServer*> _implementations;
};

} // ouinet namespace
