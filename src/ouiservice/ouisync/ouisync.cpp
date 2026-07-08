#include <boost/asio/error.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/beast/http/vector_body.hpp>
#include <expected>
#include <iterator>
#include <ouisync.hpp>
#include <ouisync/file_stream.hpp>
#include <ouisync/service.hpp>
#include "ouisync.h"
#include "error.h"
#include "ouiservice/ouisync/socket.h"
#include "util/executor.h"
#include "util/url.h"
#include "http_util.h"
#include "generic_stream.h"
#include "util/keep_alive.h"
#include "util/async.h"
#include "cache/resource.h"
#include "cache/http_store.h"

namespace ouinet::ouisync_service {

using ouisync::Session;
using ouisync::Repository;
using ouisync::File;
using ouisync::FileStream;
using ouisync::ShareToken;

// TODO: Set through cmd args as these may differ in tests and in production
static const bool SYNC_ENABLED = true;
static const bool DHT_ENABLED = true;
static const bool PEX_ENABLED = true;

template<class V> V unwrap(std::expected<V, sys::error_code> exp) {
    if (!exp.has_value()) {
        throw sys::system_error(exp.error());
    }
    return std::move(*exp);
}

void unwrap(std::expected<void, sys::error_code> exp) {
    if (!exp.has_value()) {
        throw sys::system_error(exp.error());
    }
}

void unwrap(sys::error_code ec) {
    if (ec) throw sys::system_error(ec);
}

static
Repository open_or_create_repo(Session& session, std::string_view name, const ShareToken& token, Async yield) {
    // TODO: The session should use std::string_view.
    std::string name_str(name);

    try {
        return unwrap(session.create_repository(
            name_str,
            {},
            {},
            token,
            SYNC_ENABLED, DHT_ENABLED, PEX_ENABLED,
            yield));
    }
    catch (const sys::system_error& e) {
        if (e.code() != ouisync::error::Service::already_exists) {
            throw;
        }
        // TODO: Check the returned repo corresponds to the `token`.
        return unwrap(session.find_repository(name_str, yield));
    }
}

void set_repo_defaults(Repository& repo, bool can_mount, Async yield) {
    if (can_mount) {
        unwrap(repo.mount(yield));
    }

    unwrap(repo.set_sync_enabled(true, yield));
    unwrap(repo.set_pex_enabled(true, yield));
}

File open_file(Repository& repo, std::string const& path, Async yield) {
    auto sub = repo.subscribe();
    bool is_fully_loaded = false;

    while (true) {
        auto file = repo.open_file(path, yield);
        if (file.has_value()) return std::move(*file);

        if (is_fully_loaded) {
            throw sys::system_error(file.error());
        }

        // We get STORE_ERROR when the file is there but its first block
        // has not yet been downloaded.
        if (file.error() != ouisync::error::Service::not_found &&
            file.error() != ouisync::error::Service::store_error) {
            throw sys::system_error(file.error());
        }

        auto progress = unwrap(repo.get_sync_progress(yield));

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

        unwrap(sub.async_receive(yield));
    }
}

FileStream
open_stream(Repository& repo, const std::string& path, Async yield) {
    auto file = open_file(repo, path, yield);
    return unwrap(FileStream::init(std::move(file), yield));
}

struct Ouisync::Impl {
    using Sites = std::map<std::string, std::shared_ptr<ouisync::Repository>>;

    ouisync::Service service;
    ouisync::Session session;
    std::optional<ouisync::Repository> page_index;
    Sites sites;
    bool can_mount; // Whether Ouisync was compiled with mount support

    std::shared_ptr<Repository> resolve(std::string repo_name, Async yield) {
        assert(page_index);

        auto repo_i = sites.find(repo_name);
        if (repo_i != sites.end()) {
            return repo_i->second;
        }

        auto file = open_file(page_index.value(), std::string("/") + repo_name, yield.tag("open_file"));
        auto len = unwrap(file.get_length(yield));
        auto token_vec = unwrap(file.read(0, len, yield));
        auto token = ShareToken{std::string(token_vec.begin(), token_vec.end())};

        auto repo = open_or_create_repo(session, repo_name, token, yield);
        set_repo_defaults(repo, can_mount, yield);
        auto repo_ptr = std::make_shared<Repository>(std::move(repo));

        sites[std::move(repo_name)] = repo_ptr;

        return repo_ptr;
    }
};

Ouisync::Ouisync(
    const util::AsioExecutor& exec,
    fs::path service_dir,
    std::string page_index_token,
    std::vector<asio::ip::udp::endpoint> bind) :
    _service_dir(std::move(service_dir)),
    _store_dir(_service_dir / "store"),
    _mount_dir(_service_dir / "mount"),
    _bind(std::move(bind)),
    _page_index_token(std::move(page_index_token)),
    _impl_cv(exec)
{
    fs::create_directories(_store_dir);
    fs::create_directories(_mount_dir);
}

sys::error_code Ouisync::start(Async yield)
{
    try {
        ouisync::Service service(yield.get_executor());
        unwrap(service.start(_service_dir, "ouisync", yield));

        auto session = unwrap(ouisync::Session::connect(
            yield.get_executor(),
            _service_dir,
            yield
        ));

        std::vector<std::string> bind_strs;
        std::transform(
            _bind.begin(),
            _bind.end(),
            std::back_inserter(bind_strs),
            [] (auto ep) { return util::str("quic/", ep); }
        );
        unwrap(session.bind_network(bind_strs, yield));
        unwrap(session.set_local_discovery_enabled(true, yield));

        unwrap(session.set_store_dirs({_store_dir.string()}, yield));
        auto mount_r = session.set_mount_root(_mount_dir.string(), yield);

        std::optional<ouisync::Repository> page_index;
        if (!_page_index_token.empty()) {
            page_index = open_or_create_repo(
                session,
                "page_index",
                ShareToken{_page_index_token},
                yield
            );
            set_repo_defaults(page_index.value(), mount_r.has_value(), yield);
        }

        _impl = std::make_shared<Impl>(Impl {
            std::move(service),
            std::move(session),
            std::move(page_index),
            {},
            mount_r.has_value()
        });
        _impl_cv.notify();

        return sys::error_code();
    }
    catch(sys::system_error const& e) {
        LOG_WARN(yield, " Ouisync::start exception: ", e.what());
        return e.code();
    }
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

std::expected<ouinet::Session, sys::error_code>
Ouisync::load(const CacheOuisyncRetrieveRequest& rq, Async yield) {
    try {
        if (!_impl) {
            LOG_DEBUG(yield, " Ouisync not initialized");
            throw_error(asio::error::not_connected);
        }

        if (!_impl->page_index) {
            LOG_DEBUG(yield, " Ouisync page index repository not configured");
            throw_error(asio::error::not_connected);
        }

        auto repo = _impl->resolve(rq.dht_group(), yield.tag("resolve"));

        fs::path path = cache::path_from_resource_id(cache::root_fname, rq.resource_id());

        using Reader = ouinet::cache::GenericResourceReader<FileStream>;

        auto head_file = open_stream(*repo, (path / cache::head_fname).string(), yield);

        auto head = unwrap(Reader::read_signed_head(head_file, yield));

        unwrap(head_file.close(yield));

        std::optional<FileStream> sigs_file, body_file;

        if (has_sigs(head)) {
            sigs_file.emplace(open_stream(*repo, (path / cache::sigs_fname).string(), yield));
        } else {
            sigs_file.emplace(FileStream{});
        }

        if (has_body(head)) {
            body_file.emplace(open_stream(*repo, (path / cache::body_fname).string(), yield));
        } else {
            body_file.emplace(FileStream{});
        }

        auto reader = std::make_unique<Reader>(
            std::move(head),
            std::move(*sigs_file),
            std::move(*body_file),
            std::optional<cache::Range>() // range
        );

        auto session = unwrap(ouinet::Session::create(
            std::move(reader),
            rq.method() == http::verb::head,
            yield
        ));

        session
            .response_header()
            .set(http_::response_source_hdr, http_::response_source_hdr_ouisync);

        return std::move(session);
    }
    catch (const sys::system_error& e) {
        LOG_WARN(yield, " Ouisync::serve exception: ", e.what());
        return std::unexpected(e.code());
    }
}

std::expected<std::vector<OuisyncSocket>, sys::error_code>
Ouisync::open_network_sockets(Async yield) {
    while (!_impl) {
        auto result = _impl_cv.wait(yield);
        if (!result) {
            return std::unexpected(result.error());
        }
    }

    std::vector<OuisyncSocket> sockets;
    sockets.reserve(2);

    auto v4 = OuisyncSocket::open(_impl->session, asio::ip::udp::v4(), yield);
    if (!v4) {
        return std::unexpected(v4.error());
    }

    sockets.push_back(std::move(*v4));

    auto v6 = OuisyncSocket::open(_impl->session, asio::ip::udp::v6(), yield);
    if (!v6) {
        return std::unexpected(v6.error());
    }

    sockets.push_back(std::move(*v6));

    return sockets;
}

bool Ouisync::is_running() const {
    return _impl != nullptr;
}

void Ouisync::stop() {
    auto impl = std::move(_impl);
}

} // namespace ouinet::ouisync_service

namespace ouinet::util::file_io {
    std::expected<size_t, sys::error_code>
    file_size(ouisync::FileStream& file) {
        return file.size();
    }

    std::expected<void, sys::error_code>
    fseek(ouisync::FileStream& file, size_t pos) {
        file.seek(pos);
        return {};
    }
} // namespace util::file_io
