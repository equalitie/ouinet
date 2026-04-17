#include <boost/filesystem/path.hpp>
#include <boost/beast/http/vector_body.hpp>
#include <ouisync.hpp>
#include <ouisync/service.hpp>
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

// TODO: Set through cmd args as these may differ in tests and in production
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
            {},
            {},
            token,
            SYNC_ENABLED, DHT_ENABLED, PEX_ENABLED,
            yield);
    }
    catch (const sys::system_error& e) {
        if (e.code() != ouisync::error::Service::already_exists) {
            throw;
        }
        // TODO: Check the returned repo corresponds to the `token`.
        return session.find_repository(name_str, yield);
    }
}

void set_repo_defaults(Repository& repo, bool can_mount, asio::yield_context yield) {
    if (can_mount) {
        repo.mount(yield);
    }
    repo.set_sync_enabled(true, yield);
    repo.set_pex_enabled(true, yield);
}

File open_file(Repository& repo, std::string const& path, YieldContext yield) {
    auto sub = repo.subscribe();
    bool is_fully_loaded = false;

    while (true) {
        try {
            return repo.open_file(path, yield);
        }
        catch (const sys::system_error& e) {
            if (is_fully_loaded) {
                throw;
            }

            // We get STORE_ERROR when the file is there but its first block
            // has not yet been downloaded.
            if (e.code() != ouisync::error::Service::not_found &&
                    e.code() != ouisync::error::Service::store_error) {
                throw;
            }

            auto progress = repo.get_sync_progress(yield);

            // If `progress.total == 0`, then the repo has been imported but no
            // syncing happened yet. Otherwise if `progress.total !=
            // progress.value` then the repos hasn't synced fully yet and new
            // data may still arrive.
            if (progress.total != 0 && progress.total == progress.value) {
                // Since `total == value` could have happened after
                // `open_file`, we try one more time.
                is_fully_loaded = true;
                continue;
            }

            sub.async_receive(yield);
        }
    }
}

struct Ouisync::Impl {
    using Sites = std::map<std::string, std::shared_ptr<ouisync::Repository>>;

    ouisync::Service service;
    ouisync::Session session;
    ouisync::Repository page_index;
    Sites sites;
    bool can_mount; // Whether Ouisync was compiled with mount support

    std::shared_ptr<Repository> resolve(std::string repo_name, YieldContext yield) {
        auto repo_i = sites.find(repo_name);
        if (repo_i != sites.end()) {
            return repo_i->second;
        }

        auto file = open_file(page_index, std::string("/") + repo_name, yield.tag("open_file"));
        auto len = file.get_length(yield);
        auto token_vec = file.read(0, len, yield);
        auto token = ShareToken{std::string(token_vec.begin(), token_vec.end())};

        auto repo = open_or_create_repo(session, repo_name, token, yield);
        set_repo_defaults(repo, can_mount, yield);
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
    bool can_mount = true;
    try {
        session.set_mount_root(_mount_dir.string(), yield);
    } catch (std::exception& e) {
        can_mount = false;
    }
    session.set_local_discovery_enabled(true, yield);

    auto page_index = open_or_create_repo(session, "page_index", ShareToken{_page_index_token}, yield);
    set_repo_defaults(page_index, can_mount, yield);

    _impl = std::make_shared<Impl>(Impl {
        std::move(service),
        std::move(session),
        std::move(page_index),
        {},
        can_mount
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

    util::http_reply(con, rs, yield);
}

static bool has_body(http::response_header<> const& hdr) {
    auto i = hdr.find("X-Ouinet-Data-Size");
    if (i == hdr.end()) {
        return false;
    }
    // TODO: Parse integer
    if (i->value() == "0") {
        return false;
    }
    return true;
}

static bool has_sigs(http::response_header<> const& hdr) {
    auto i = hdr.find("X-Ouinet-Data-Size");
    if (i == hdr.end()) {
        return false;
    }
    return true;
}

ouinet::Session Ouisync::load(const CacheOuisyncRetrieveRequest& rq, YieldContext yield_) {
    auto yield = yield_.throwing();

    try {
        if (!_impl) {
            throw_error(asio::error::not_connected);
        }

        auto repo = _impl->resolve(rq.dht_group(), yield.tag("resolve"));

        // TODO: Use constants from http_store.cpp instead of these hardcoded
        // strings
        fs::path root = "data-v4";
        fs::path path = cache::path_from_resource_id(root, rq.resource_id());

        using Reader = ouinet::cache::GenericResourceReader<OuisyncFile>;

        Cancel cancel;
        auto head_file = OuisyncFile::init(open_file(*repo, (path / "head").string(), yield), yield);
        auto head = Reader::read_signed_head(head_file, cancel, yield);
        head_file.close(yield);

        std::optional<OuisyncFile> sigs_file, body_file;

        if (has_sigs(head)) {
            sigs_file.emplace(OuisyncFile::init(open_file(*repo, (path / "sigs").string(), yield), yield));
        } else {
            sigs_file.emplace(OuisyncFile::empty(yield.get_executor()));
        }
        if (has_body(head)) {
            body_file.emplace(OuisyncFile::init(open_file(*repo, (path / "body").string(), yield), yield));
        } else {
            body_file.emplace(OuisyncFile::empty(yield.get_executor()));
        }

        auto reader = std::make_unique<Reader>(
            std::move(head),
            std::move(*sigs_file),
            std::move(*body_file),
            boost::optional<cache::Range>() // range
        );

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
