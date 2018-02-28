#include "ouiservice.h"

#include "or_throw.h"

namespace ouinet {

OuiServiceServer::OuiServiceServer(asio::io_service& ios):
	_ios(ios),
	_is_listening(false)
{}

void OuiServiceServer::add(std::unique_ptr<OuiServiceImplementationServer>&& implementation)
{
	_implementations.push_back(std::move(implementation));
}

void OuiServiceServer::start_listen(asio::yield_context yield)
{
	if (_is_listening) {
		// TODO: Figure out how to use custom error codes
		or_throw(yield, sys::errc::invalid_argument);
		return;
	}
	
	// TODO: This needs to happen in parallel
	for (auto it : _implementations) {
		sys::error_code ec;
		it->start_listen(yield[ec]);
		if (!ec) {
			_listening_implementations.push_back(*it);
		}
	}
	
	if (_listening_implementations.empty()) {
		// TODO: Figure out how to use custom error codes
		or_throw(yield, sys::errc::connection_refused);
		return;
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

GenericConnection OuiServiceServer::accept(asio::yield_context yield)
{
	using Ret = Result<std::unique_ptr<GenericConnection>>;
	
	if (!_is_listening) {
		// TODO: Figure out how to use custom error codes
		// TODO: Add empty-value support to GenericConnection
		return or_throw<GenericConnection>(yield, sys::errc::invalid_argument);
	}
	
	// Try accept()ing each of _listening_implementations in parallel
	// When the first one returns successfully, cancel all the others, and return that one.
	// If all fail, return an error.
}

void OuiServiceServer::cancel_accept(asio::yield_context yield)
{
	// Cancel the active accept() call
}



} // ouinet namespace
