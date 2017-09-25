#pragma once

#include <boost/system/error_code.hpp>
#include <iostream>

#include "namespaces.h"

namespace ouinet {

// Report a failure
static
void fail(sys::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

} // ouinet namespace
