#include <boost/filesystem/path.hpp>
#include <boost/beast/http/vector_body.hpp>
#include <ouisync.hpp>
#include <ouisync/service.hpp>
#include <ouisync/subscription.hpp>
#include "ouisync.h"
#include "error.h"
#include "util/url.h"
#include "http_util.h"
#include "generic_stream.h"
#include "util/keep_alive.h"
#include "cache/cache_entry.h"
#include "ouiservice/ouisync/file.h"
#include "cache/resource.h"
#include "cache/http_store.h"

namespace ouinet::ouisync_service {

using ouisync::Session;
using ouisync::Repository;
using ouisync::File;
using ouisync::ShareToken;
using ouisync::RepositorySubscription;

static const ShareToken INDEX_TOKEN{"https://ouisync.net/r#AwEglsib07NXLGQJsiLMXQnYheM4rusMFBPcEYJyNe5YYSAgOafq4GHj1NOG-E2UY88ehe4yX4gBGH9X4beIAv7jDcE?name=page_index"};
static const bool SYNC_ENABLED = true;
static const bool DHT_ENABLED = true;
static const bool PEX_ENABLED = true;

static
Repository open_or_create_repo(Session& session, std::string_view name, const ShareToken& token, asio::yield_context yield) {
    // TODO: The session should use std::string_view.
    std::string name_str(name);

    try {
        return session.create_repository(
            name_str,
            yield,
            {},
            {},
            token,
            SYNC_ENABLED, DHT_ENABLED, PEX_ENABLED);
    }
    catch (const sys::system_error& e) {
        if (e.code() != ouisync::error::ALREADY_EXISTS) {
            throw;
        }
        // TODO: Check the returned repo corresponds to the `token`.
        return session.find_repository(name_str, yield);
    }
}

void set_repo_defaults(Repository& repo, asio::yield_context yield) {
    repo.mount(yield);
    repo.set_sync_enabled(true, yield);
    repo.set_pex_enabled(true, yield);
}

File open_file(Repository& repo, std::string const& path, asio::yield_context yield) {
    RepositorySubscription sub;
    sub.subscribe(repo, yield);

    while (true) {
        try {
            return repo.open_file(path, yield);
        }
        catch (const sys::system_error& e) {
            // We get STORE_ERROR when the file is there but it's first block
            // has not yet been downloaded.
            if (e.code() != ouisync::error::NOT_FOUND &&
                    e.code() != ouisync::error::STORE_ERROR) {
                throw;
            }
            // TODO: Break if the repo has been fully synced
            sub.state_changed(yield);
        }
    }
}

struct Ouisync::Impl {
    using Sites = std::map<std::string, std::shared_ptr<ouisync::Repository>>;

    ouisync::Service service;
    ouisync::Session session;
    ouisync::Repository page_index;
    Sites sites;

    std::shared_ptr<Repository> resolve(std::string repo_name, asio::yield_context yield) {
        auto repo_i = sites.find(repo_name);
        if (repo_i != sites.end()) {
            return repo_i->second;
        }

        auto file = open_file(page_index, std::string("/") + repo_name, yield);
        auto len = file.get_length(yield);
        auto token_vec = file.read(0, len, yield);
        auto token = ShareToken{std::string(token_vec.begin(), token_vec.end())};

        auto repo = open_or_create_repo(session, repo_name, token, yield);
        set_repo_defaults(repo, yield);
        auto repo_ptr = std::make_shared<Repository>(std::move(repo));

        sites[std::move(repo_name)] = repo_ptr;

        return repo_ptr;
    }
};

Ouisync::Ouisync(fs::path service_dir) :
    _service_dir(std::move(service_dir)),
    _store_dir(_service_dir / "store"),
    _mount_dir(_service_dir / "mount")
{
    fs::create_directories(_store_dir);
    fs::create_directories(_mount_dir);
}

void Ouisync::start(asio::yield_context yield)
{
    ouisync::Service service(yield.get_executor());
    service.start(_service_dir, "ouisync", yield);

    auto session = ouisync::Session::connect(_service_dir, yield);

    session.bind_network({"quic/0.0.0.0:0"}, yield);
    session.set_store_dir(_store_dir.string(), yield);
    session.set_mount_root(_mount_dir.string(), yield);
    session.set_local_discovery_enabled(true, yield);

    auto page_index = open_or_create_repo(session, "page_index", INDEX_TOKEN, yield);
    set_repo_defaults(page_index, yield);

    _impl = std::make_shared<Impl>(Impl {
        std::move(service),
        std::move(session),
        std::move(page_index),
        {}
    });
}

template<class Request>
void reply_error(const Request& rq, sys::system_error e, GenericStream& con, YieldContext yield) {
    std::stringstream ss;
    ss << "Error: " << e.what() << "\n";
    auto rs = util::http_error(
        util::get_keep_alive(rq),
        http::status::bad_request,
        OUINET_CLIENT_SERVER_STRING,
        "",
        ss.str()
    );

    util::http_reply(con, rs, yield.native());
}

void Ouisync::serve(GenericStream& con, const http::request_header<>& rq, YieldContext yield_) {
    auto yield = yield_.throwing();

    try {
        if (!_impl) {
            throw_error(asio::error::not_connected);
        }

        auto url = util::Url::from(rq.target());

        if (!url) {
            throw_error(asio::error::invalid_argument);
        }

        auto repo = _impl->resolve(url->host, yield.native());

        //auto key = key_from_http_req(rq).value();
        auto resource_id = cache::ResourceId::from_url(rq.target(), yield).value();

        // TODO: Use constants from http_store.cpp instead of these hardcoded
        // strings
        fs::path root = "data-v3";
        //fs::path path = root / cache::relative_path_from_key(key);
        fs::path path = cache::path_from_resource_id(root, resource_id);
        auto head_file = OuisyncFile::init(open_file(*repo, (path / "head").string(), yield.native()), yield.native());
        auto sigs_file = OuisyncFile::init(open_file(*repo, (path / "sigs").string(), yield.native()), yield.native());
        auto body_file = OuisyncFile::init(open_file(*repo, (path / "body").string(), yield.native()), yield.native());

        using Reader = ouinet::cache::GenericResourceReader<OuisyncFile>;

        auto reader = std::make_unique<Reader>(
            std::move(head_file),
            std::move(sigs_file),
            std::move(body_file),
            boost::optional<cache::Range>() // range
        );

        // TODO: Use cancel
        Cancel cancel;
        auto session = ouinet::Session::create(
            std::move(reader),
            rq.method() == http::verb::head,
            cancel,
            yield.native()
        );

        session.flush_response(con, cancel, yield.native());
    }
    catch (const sys::system_error& e) {
        LOG_WARN("Ouisync::serve exception: ", e.what());
        sys::error_code ec;
        reply_error(rq, e, con, yield_[ec]);
        if (ec) {
            return or_throw(yield_, e.code());
        }
    }
}

bool Ouisync::is_running() const {
    return _impl != nullptr;
}

void Ouisync::stop() {
    auto impl = std::move(_impl);
}

} // namespace ouinet::ouisync_service
