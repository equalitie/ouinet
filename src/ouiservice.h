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
	
	virtual GenericConnection accept(asio::yield_context yield) = 0;
    // NOTE: Requiring that cancelation is an async OP which only finished once
    // the cancelled OP is done greatly complicates it's implementation for
    // little (if any?) benefit.
    //
    // For a gist think of the answer to this question: "what is the correct
    // behavior if someone calls accept while cancel_accept is taking place?"
    //
    // Another argument being that it's unlike any other interface that Asio
    // provides. Thus once again unnecessarily introducing our own untested
    // idioms.
	virtual void cancel_accept(asio::yield_context yield) = 0;

    virtual bool is_accepting() = 0;
};

class OuiServiceServer
{
	public:
	OuiServiceServer(asio::io_service& ios);
	
	void add(OuiServiceImplementationServer* implementation);
	
	void start_listen(asio::yield_context);
	void stop_listen(asio::yield_context yield);
	
	GenericConnection accept(asio::yield_context yield);
	void cancel_accept(asio::yield_context yield);
	
	private:
	asio::io_service& _ios;
	std::vector<OuiServiceImplementationServer*> _implementations;
	bool _is_listening;
	bool _is_canceling_listen;
    bool _is_accepting;
    bool _is_canceling_accept;
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
	
	//void Result<std::unique_ptr<GenericConnection>>(asio::yield_context yield) = 0;
	void cancel_connect(asio::yield_context yield);
	
	private:
	asio::io_service& _ios;
	std::vector<OuiServiceImplementationServer*> _implementations;
};

} // ouinet namespace
