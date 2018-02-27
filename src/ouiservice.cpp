#include "ouiservice.h"

namespace ouinet {

OuiServiceServer::OuiServiceServer(asio::io_service& ios):
	_ios(ios),
	_is_listening(false)
{}

void OuiServiceServer::add(OuiServiceImplementationServer* implementation)
{
	_implementations.push_back(implementation);
}

sys::error_code OuiServiceServer::start_listen(asio::yield_context yield)
{
	if (_is_listening) {
		// TODO: Figure out how to use custom error codes
		return sys::make_error_code(sys::errc::invalid_argument);
	}
	
	// TODO: This needs to happen in parallel
	for (auto it : _implementations) {
		sys::error_code error = it->start_listen(yield);
		if (!error) {
			_listening_implementations.push_back(*it);
		}
	}
	
	if (_listening_implementations.empty()) {
		// TODO: Figure out how to use custom error codes
		return sys::make_error_code(sys::errc::connection_refused);
	}
	
	_is_listening = true;
	
	return sys::error_code();
}

void OuiServiceServer::stop_listen(asio::yield_context yield)
{
	for (auto it : _listening_implementations) {
		it->stop_listen(yield);
	}
	
	_listening_implementations.clear();
	_is_listening = false;
}

Result<std::unique_ptr<GenericConnection>> OuiServiceServer::accept(asio::yield_context yield)
{
	using Ret = Result<std::unique_ptr<GenericConnection>>;
	
	if (!_is_listening) {
		// TODO: Figure out how to use custom error codes
		return Ret::make_error(sys::make_error_code(sys::errc::invalid_argument));
	}
	
	
	
	
}

void OuiServiceServer::cancel_accept(asio::yield_context yield)
{
}



} // ouinet namespace
