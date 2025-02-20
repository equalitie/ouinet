#pragma once

#include <boost/asio/spawn.hpp>
#include <string>
#include <vector>
#include <string_view>
#include "util/executor.h"
#include "cxx/async_callback.h"

// Forward declarations for lib.rs.h
namespace ouinet::metrics::bridge {
    struct CxxRecordProcessor;
    struct CxxOneShotSender;
}

#include "rust/src/lib.rs.h"
#include "rust/cxx.h"

namespace ouinet::metrics::bridge {

using namespace std;

struct CxxRecordProcessor {
    util::AsioExecutor executor;

    // Function provided by the user to process the record.
    AsyncCallback async_callback;

    CxxRecordProcessor(util::AsioExecutor executor, AsyncCallback async_callback)
        : executor(move(executor))
        , async_callback(move(async_callback)) {}

    void execute( rust::String record_name
                , rust::String record_content
                , rust::Box<bridge::CxxOneShotSender> on_finish) const;
};


} // namespace
