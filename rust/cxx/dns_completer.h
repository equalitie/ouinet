#pragma once

#include "ouinet-rs/src/dns.rs.h"

namespace ouinet::dns::bridge {

class Completer {
public:

    // Completer();

    void complete(rust::Vec<IpAddress> addresses) const;
};

} // namespace ouinet::dns::bridge
