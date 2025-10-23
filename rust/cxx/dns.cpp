#include "dns.h"

namespace ouinet::dns {

using namespace boost::asio;
using namespace boost::system;

Resolver::Resolver() : _impl(bridge::new_resolver()) {}

Resolver::Output Resolver::resolve(const std::string& name, yield_context yield) {
    return async_initiate<yield_context, void(error_code, Output)> (
        [&name, this] (auto completion_handler, auto exec) {
            using CompletionHandler = decltype(completion_handler);

            // `std::function` in `Completer` requires the lambda to be
            // copyable, but `completion_handler` isn't, so we need to wrap it
            // in a shared pointer.
            auto completion_handler_p =
                std::make_shared<CompletionHandler>(std::move(completion_handler));

            _impl->resolve(name, std::make_unique<bridge::Completer>(
                [ work_guard = make_work_guard(std::move(exec)),
                  completion_handler = std::move(completion_handler_p)
                ] (bridge::Completer::Result&& result) {
                    post(work_guard.get_executor(), [
                        h = std::move(*completion_handler),
                        r = std::move(result)
                    ] () mutable {
                        if (r.has_value()) {
                            h(error_code{}, std::move(r.value()));
                        } else {
                            h(r.error(), Output{});
                        }
                    });
                }
            ));
        },
        yield,
        yield.get_executor()
    );
}

namespace bridge {
    ip::address convert(IpAddress);

    Completer::Completer(Function&& function) : _function(std::move(function)) {}
    Completer::Completer(const Function& function) : _function(function) {}

    void Completer::on_success(rust::Vec<IpAddress> addresses) const {
        std::vector<ip::address> output;
        std::transform(addresses.begin(), addresses.end(), std::back_inserter(output), convert);

        _function(std::move(output));
    }

    void Completer::on_failure(rust::String error) const {
        // TODO
        _function(error::host_unreachable);
    }

    ip::address convert(IpAddress input) {
        auto addr = ip::address_v6(input.octets);

        if (addr.is_v4_mapped()) {
            return ip::make_address_v4(ip::v4_mapped, addr);
        } else {
            return addr;
        }
    }
}
}

