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

File open_file(Repository& repo, std::string const& path, YieldContext yield) {
    RepositorySubscription sub;
    sub.subscribe(repo, yield.native());

    while (true) {
        try {
            return repo.open_file(path, yield.native());
        }
        catch (const sys::system_error& e) {
            // We get STORE_ERROR when the file is there but it's first block
            // has not yet been downloaded.
            if (e.code() != ouisync::error::NOT_FOUND &&
                    e.code() != ouisync::error::STORE_ERROR) {
                throw;
            }
            // TODO: Break if the repo has been fully synced
            sub.state_changed(yield.native());
        }
    }
}

struct Ouisync::Impl {
    using Sites = std::map<std::string, std::shared_ptr<ouisync::Repository>>;

    ouisync::Service service;
    ouisync::Session session;
    ouisync::Repository page_index;
    Sites sites;

    std::shared_ptr<Repository> resolve(std::string repo_name, YieldContext yield) {
        auto repo_i = sites.find(repo_name);
        if (repo_i != sites.end()) {
            return repo_i->second;
        }

        auto file = open_file(page_index, std::string("/") + repo_name, yield.tag("open_file"));
        auto len = file.get_length(yield.native());
        auto token_vec = file.read(0, len, yield.native());
        auto token = ShareToken{std::string(token_vec.begin(), token_vec.end())};

        auto repo = open_or_create_repo(session, repo_name, token, yield.native());
        set_repo_defaults(repo, yield.native());
        auto repo_ptr = std::make_shared<Repository>(std::move(repo));

        sites[std::move(repo_name)] = repo_ptr;

        return repo_ptr;
    }
};

Ouisync::Ouisync(fs::path service_dir, std::string page_index_token) :
    _service_dir(std::move(service_dir)),
    _store_dir(_service_dir / "store"),
    _mount_dir(_service_dir / "mount"),
    _page_index_token(std::move(page_index_token))
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
    session.set_store_dirs({_store_dir.string()}, yield);
    session.set_mount_root(_mount_dir.string(), yield);
    session.set_local_discovery_enabled(true, yield);

    auto page_index = open_or_create_repo(session, "page_index", ShareToken{_page_index_token}, yield);
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

ouinet::Session Ouisync::load(const CacheOuisyncRetrieveRequest& rq, YieldContext yield_) {
    auto yield = yield_.throwing();

    try {
        if (!_impl) {
            throw_error(asio::error::not_connected);
        }

        auto url = util::Url::from(rq.target());

        if (!url) {
            throw_error(asio::error::invalid_argument);
        }

        auto repo = _impl->resolve(rq.dht_group(), yield.tag("resolve"));

        // TODO: Use constants from http_store.cpp instead of these hardcoded
        // strings
        fs::path root = "data-v3";
        fs::path path = cache::path_from_resource_id(root, rq.resource_id());
        auto head_file = OuisyncFile::init(open_file(*repo, (path / "head").string(), yield), yield.native());
        auto sigs_file = OuisyncFile::init(open_file(*repo, (path / "sigs").string(), yield), yield.native());
        auto body_file = OuisyncFile::init(open_file(*repo, (path / "body").string(), yield), yield.native());

        using Reader = ouinet::cache::GenericResourceReader<OuisyncFile>;

        auto reader = std::make_unique<Reader>(
            std::move(head_file),
            std::move(sigs_file),
            std::move(body_file),
            boost::optional<cache::Range>() // range
        );

        Cancel cancel;
        auto session = ouinet::Session::create(
            std::move(reader),
            rq.method() == http::verb::head,
            cancel,
            yield
        );

        session
            .response_header()
            .set(http_::response_source_hdr, http_::response_source_hdr_ouisync);

        return session;
    }
    catch (const sys::system_error& e) {
        LOG_WARN(yield_, " Ouisync::serve exception: ", e.what());
        return or_throw<ouinet::Session>(yield_, e.code());
    }
}

bool Ouisync::is_running() const {
    return _impl != nullptr;
}

void Ouisync::stop() {
    auto impl = std::move(_impl);
}

} // namespace ouinet::ouisync_service
