#include "native-lib.hpp"
#include <TargetConditionals.h>

#include <string>
//#include <android/log.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <client.h>
#include <client_config.h>
#include <util/signal.h>
#include <util/crypto.h>
#include <condition_variable>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

//#include "debug.h"
//#include "std_scoped_redirect.h"

using namespace std;
using ouinet::ClientConfig;

/*
struct Defer {
    Defer(function<void()> f) : _f(move(f)) {}
    ~Defer() { _f(); }
    function<void()> _f;
};
*/

// g_client is only accessed from the g_client_thread.
std::unique_ptr<ouinet::Client> g_client;
ouinet::asio::io_service g_ios;

int NativeLib::getClientState()
{
    // TODO: Avoid needing to keep this in sync by hand.
    if (!g_client)
        return g_ios.stopped() ? 6 /* stopped */ : 0 /* created */;
    switch (g_client->get_state()) {
    case ouinet::Client::RunningState::Created:  return 0;
    case ouinet::Client::RunningState::Failed:   return 1;
    case ouinet::Client::RunningState::Starting: return 2;
    case ouinet::Client::RunningState::Degraded: return 3;
    case ouinet::Client::RunningState::Started:  return 4;
    case ouinet::Client::RunningState::Stopping: return 5;
    case ouinet::Client::RunningState::Stopped:  return 6;
    }
    return -1;
}

std::string NativeLib::helloOuinet()
{
    return std::string("Hello Ouinet, this libary was definitely compiled inside of the ouinet cmake build system, cool");
}
