#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <sys/wait.h>

#include "client.h"

namespace ouinet::test::util {

pid_t fork(std::string name, std::vector<std::string> const& args) {
    std::vector<const char*> argv;

    argv.push_back(name.data());
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    assert(pid >= 0);
    if (pid == 0) {
        execvp(name.data(), (char* const*) argv.data());
        exit(0);
    }
    return pid;
}

template<class Token>
auto execute(boost::asio::any_io_executor exec, std::string name, std::vector<std::string> args, Token token) {
    return boost::asio::async_initiate<
            Token,
            void(boost::system::error_code)
        >([
            name = std::move(name),
            args = std::move(args),
            work_guard = boost::asio::make_work_guard(exec)
        ] (auto handler) {
            auto thread = std::thread([
                name = std::move(name),
                args = std::move(args),
                handler = std::move(handler),
                work_guard = std::move(work_guard)
            ] () mutable {
                pid_t pid = fork(std::move(name), std::move(args));
                int status = 0;
                waitpid(pid, &status, 0);
                sys::error_code ec;
                ec.assign(status, boost::system::system_category());
                boost::asio::post(
                    work_guard.get_executor(),
                    [ec, h = std::move(handler)] () mutable {
                        h(ec);
                    });
            });
            thread.detach();
        },
        token);
}

template<class Token>
auto start_browser_for_client(boost::asio::any_io_executor exec, Client const& client, Token token) {
    using namespace std::string_literals;

    auto profile_dir = client.config().repo_root() / "browser_profile";
    fs::create_directory(profile_dir);

    std::stringstream ss;
    ss << client.get_proxy_endpoint();
    auto proxy_ep_str = ss.str();

    std::vector<std::string> args = {
        "--user-data-dir="s + profile_dir.string(),
        "--proxy-server="s  + "http=" + proxy_ep_str + ";https=" + proxy_ep_str,
        // TODO: Would have been better to install Client's CA cert to the
        // browser prior to starting it.
        "--ignore-certificate-errors"s
    };

    return execute(std::move(exec), "chromium", std::move(args), std::move(token));
}

} // namespace
