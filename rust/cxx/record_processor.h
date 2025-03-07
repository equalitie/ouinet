#pragma once

#include <boost/asio/spawn.hpp>
#include <string>
#include <vector>
#include <string_view>
#include "util/executor.h"
#include "cxx/async_callback.h"

// Forward declarations for bridge.rs.h
namespace ouinet::metrics::bridge {
    struct CxxRecordProcessor;
    struct CxxOneShotSender;
}

#include "rust/src/bridge.rs.h"
#include "rust/cxx.h"

namespace ouinet::metrics::bridge {

struct CxxRecordProcessor {
    util::AsioExecutor executor;

    // Function provided by the user to process the record.
    AsyncCallback async_callback;

    CxxRecordProcessor(util::AsioExecutor executor, AsyncCallback async_callback)
        : executor(std::move(executor))
        , async_callback(std::move(async_callback)) {}

    void execute( rust::String record_name
                , rust::String record_content
                , rust::Box<bridge::CxxOneShotSender> on_finish) const;
};


} // namespace
