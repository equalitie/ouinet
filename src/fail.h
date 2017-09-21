#pragma once

namespace ouinet {

// Report a failure
static
void fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

} // ouinet namespace
