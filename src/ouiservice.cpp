#include "ouiservice.h"
#include "or_throw.h"
#include <boost/asio.hpp>
#include "namespaces.h"
#include "blocker.h"

#include "or_throw.h"

namespace ouinet {

OuiServiceServer::OuiServiceServer(asio::io_service& ios):
	_ios(ios),
	_is_listening(false),
	_is_canceling_listen(false),
	_is_accepting(false),
	_is_canceling_accept(false)
{}

void OuiServiceServer::add(std::unique_ptr<OuiServiceImplementationServer> implementation)
{
	_implementations.push_back(std::move(implementation));
}

// NOTE Leaky abstraction: Even if we do start_listen in parallel, this API
// forces all the implementations to wait for the slowest one.
void OuiServiceServer::start_listen(asio::yield_context yield)
{
	if (_is_listening) {
		return or_throw(yield, asio::error::invalid_argument);
	}
	
	Blocker blocker(_ios);
	
	for (auto& it : _implementations) {
		asio::spawn(_ios, [this, implementation = it.get(), b = blocker.make_block()] (asio::yield_context yield) {
			sys::error_code ec;
			implementation->start_listen(yield[ec]);
			if (ec) return;
			_listening_implementations.push_back(implementation);
		});
	}
	
	blocker.wait(yield);
	
	if (_listening_implementations.empty()) {
		// TODO: Figure out how to use custom error codes
		return or_throw(yield, asio::error::connection_refused);
	}
	
	_is_listening = true;
}

void OuiServiceServer::stop_listen(asio::yield_context yield)
{
	for (auto it : _listening_implementations) {
		it->stop_listen(yield);
	}
	
	_listening_implementations.clear();
	_is_listening = false;
}

// NOTE: Leaky abstraction: In the original architecture, each individual
// transoport started to serve connections right after it accepted it. With
// this API, each time a connection is accepted, it triggers an unnecessary
// round of canceling acceptation on the rest of the transport and only once
// each one is of them is cancelled, the caller of this function can make use
// of the newly accepted connection.
//
// The speed at which servers can accept connections is usually something that
// devs prefer to optimize extensivel, not the other way around.
GenericConnection OuiServiceServer::accept(asio::yield_context yield)
{
	if (!_is_listening) {
		// TODO: Figure out how to use custom error codes
		return or_throw<GenericConnection>(yield, asio::error::invalid_argument);
	}
	
	assert(!_is_accepting);
	assert(!_is_canceling_accept);
	_is_accepting = true;
	
	Blocker blocker(_ios);
	
	bool accepted = false;
	sys::error_code first_error; // Only set if accepted == false
	GenericConnection connection;
	
	for (auto it : _listening_implementations) {
		asio::spawn(_ios, [
			  this
			, it
			, &connection
			, &accepted
			, &first_error
			, b = blocker.make_block()
		] (asio::yield_context yield) {
			sys::error_code ec;
			connection = it->accept(yield[ec]);
			if (accepted) return;
			if (ec && !first_error) first_error = ec;
			if (ec) return;
			accepted = true;
			for (auto it_ : _listening_implementations) {
				if (it == it_ || !it_->is_accepting()) continue;
				sys::error_code ec;
				it_->cancel_accept();
			}
		});
	}
	
	blocker.wait(yield);
	
	_is_accepting = false;
	return or_throw(yield, first_error, std::move(connection));
}

void OuiServiceServer::cancel_accept()
{
	assert(!_is_canceling_accept);
	_is_canceling_accept = true;
	
	if (!_is_accepting) return;
	
	for (auto it : _listening_implementations) {
		if (!it->is_accepting()) continue;
		it->cancel_accept();
	}
	
	_is_canceling_accept = false;
}

OuiServiceClient::OuiServiceClient(asio::io_service& ios):
	_ios(ios)
{}

void OuiServiceClient::add(std::unique_ptr<OuiServiceImplementationClient> implementation)
{
	assert(_implementations.empty());
	_implementations.push_back(std::move(implementation));
}

GenericConnection OuiServiceClient::connect(asio::yield_context yield)
{
	assert(_implementations.size() == 1);
	return _implementations[0]->connect(yield);
}

void OuiServiceClient::cancel_connect()
{
	assert(_implementations.size() == 1);
	_implementations[0]->cancel_connect();
}

} // ouinet namespace
