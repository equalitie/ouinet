#include "constants.h"
#include "dns.h"
#include "util/address.h"
#include "parse/number.h"

namespace ouinet::dns {

using namespace boost::asio;
using namespace boost::system;
using boost::source_location;

using bridge::Error;

const char* ErrorCategory::name() const noexcept {
    return "dns_error_category";
}

std::string ErrorCategory::message(int ev) const {
    switch (static_cast<Error>(ev)) {
    case Error::Ok:
        return "Ok";
    case Error::NotFound:
        return "Not found";
    case Error::Busy:
        return "Busy";
    case Error::Cancelled:
        return "Cancelled";
    case Error::Other:
    default:
        return "Other error";
    }
}

ErrorCategory error_category;

namespace bridge {

BasicCompleter::BasicCompleter(cancellation_slot&& cancellation_slot) :
    _cancellation_slot(std::move(cancellation_slot))
{}

void BasicCompleter::on_cancel(rust::Box<CancellationToken> token) {
    if (_cancellation_slot.is_connected()) {
        _cancellation_slot.assign([token = std::move(token)](auto cancellation_type) {
            token->cancel();
        });
    }
}

template<typename CompletionHandler>
class Completer : public BasicCompleter {
public:
    using WorkGuard = executor_work_guard<typename associated_executor<CompletionHandler>::type>;

    Completer(cancellation_slot&& cancellation_slot, CompletionHandler&& handler) :
        BasicCompleter(std::move(cancellation_slot)),
        _work_guard(make_work_guard(handler)),
        _handler(std::move(handler))
    {}

    void complete(Error error, rust::Vec<IpAddress> ips) final {
        static constexpr boost::source_location source_location = BOOST_CURRENT_LOCATION;
        auto ec = error_code(error, &source_location);

        std::vector<ip::address> asio_ips;
        std::transform(
            ips.begin(),
            ips.end(),
            std::back_inserter(asio_ips),
            [](auto input) {
                auto addr = ip::address_v6(input.octets);

                if (addr.is_v4_mapped()) {
                    return ip::address(ip::make_address_v4(ip::v4_mapped, addr));
                } else {
                    return ip::address(addr);
                }
            }
        );

        post(_work_guard.get_executor(), [
            handler = std::move(_handler),
            ec,
            asio_ips = std::move(asio_ips)
        ] () mutable {
            handler(ec, std::move(asio_ips));
        });

        _work_guard.reset();
    }

private:
    WorkGuard _work_guard;
    CompletionHandler _handler;
};

} // namespace bridge


Resolver::Resolver()
    : _impl(bridge::new_resolver(default_config())){
}

Resolver::Resolver(const Config& config)
    : _impl(bridge::new_resolver(config)) {
}

Config Resolver::default_config() {
    Config cfg{};
    for (auto proto_name : dns_default_protocols ) {
        cfg.protocols.emplace_back(
            bridge::str_to_proto(proto_name)
        );
    }
    return cfg;
}

std::string Resolver::protos_to_str(rust::Vec<bridge::Protocol> protos) {
    std::string proto_str;
    for (const auto proto : protos) {
        proto_str +=
            (proto_str.empty() ? "" : ", ") +
            std::string(proto_to_str(proto));
    }
    return proto_str;
}

Resolver::Output Resolver::resolve(const std::string& name, yield_context yield) {
    auto cancellation_slot = yield.get_cancellation_slot();

    return async_initiate<yield_context, void(error_code, Output)> (
        [
            this,
            &name,
            cancellation_slot = std::move(cancellation_slot)
        ] (auto completion_handler) mutable {
            if (!_impl) {
                completion_handler(error::operation_aborted, {});
                return;
            }

            using CompletionHandler = decltype(completion_handler);
            using Completer = bridge::Completer<CompletionHandler>;

            (**_impl).resolve(
                name,
                std::make_unique<Completer>(
                    std::move(cancellation_slot),
                    std::move(completion_handler)
                )
            );
        },
        yield
    );
}


TcpLookup Resolver::resolve( const std::string& host
                           , const uint16_t port
                           , const Cancel& cancel
                           , YieldContext yield)
{
    using TcpEndpoint = TcpLookup::endpoint_type;

    // Build and return lookup if `host` is already a network address.
    {
        sys::error_code e;
        auto addr = asio::ip::make_address(host, e);
        if (!e) return TcpLookup::create( TcpEndpoint{std::move(addr), port}
                                        , host
                                        , std::to_string(port));
    }

    sys::error_code ec;
    const auto answers46 = resolve(host, yield[ec].tag("resolve host"));

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<TcpLookup>(yield, ec);

    const util::AddrsAsEndpoints<util::Answers, TcpEndpoint> eps{answers46, port};
    return TcpLookup::create( eps.begin()
                            , eps.end()
                            , host
                            , std::to_string(port));
}

void Resolver::close() {
    _impl.reset();
}

} // namespace ouinet::dns

