#include "http_store.h"

#include <boost/optional.hpp>

#include "../or_throw.h"
#include "../util/file_io.h"
#include "../util/variant.h"

namespace ouinet { namespace cache {

static const fs::path head_fname = "head";
static const fs::path body_fname = "body";
static const fs::path sigs_fname = "sigs";

class SplittedWriter {
public:
    SplittedWriter(const fs::path& dirp, const asio::executor& ex)
        : dirp(dirp), ex(ex) {}

private:
    const fs::path& dirp;
    const asio::executor& ex;

    boost::optional<asio::posix::stream_descriptor> headf, bodyf, sigsf;

    inline
    asio::posix::stream_descriptor
    create_file(const fs::path& fname, Cancel cancel, sys::error_code& ec)
    {
        auto f = util::file_io::open_or_create(ex, dirp / fname, ec);
        if (cancel) ec = asio::error::operation_aborted;
        return f;
    }

public:
    void
    async_write_part(http_response::Head, Cancel cancel, asio::yield_context yield)
    {
        assert(!headf);

        sys::error_code ec;
        auto hf = create_file(head_fname, cancel, ec);
        return_or_throw_on_error(yield, cancel, ec);
        headf = std::move(hf);
        // TODO: actually store
    }

    void
    async_write_part(http_response::ChunkHdr, Cancel cancel, asio::yield_context yield)
    {
        if (sigsf) return;

        sys::error_code ec;
        auto sf = create_file(sigs_fname, cancel, ec);
        return_or_throw_on_error(yield, cancel, ec);
        sigsf = std::move(sf);
        // TODO: actually store
    }

    void
    async_write_part(std::vector<uint8_t>, Cancel cancel, asio::yield_context yield)
    {
        if (bodyf) return;

        sys::error_code ec;
        auto bf = create_file(body_fname, cancel, ec);
        return_or_throw_on_error(yield, cancel, ec);
        bodyf = std::move(bf);
        // TODO: actually store
    }

    void
    async_write_part(http_response::Trailer, Cancel cancel, asio::yield_context yield)
    {
        assert(headf);
        // TODO: actually store
    }
};

void
http_store( http_response::AbstractReader& reader, const fs::path& dirp
          , const asio::executor& ex, Cancel cancel, asio::yield_context yield)
{
    SplittedWriter writer(dirp, ex);

    while (true) {
        sys::error_code ec;

        auto part = reader.async_read_part(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
        if (!part) break;

        util::apply(std::move(*part), [&](auto&& p) {
            writer.async_write_part(std::move(p), cancel, yield[ec]);
        });
        return_or_throw_on_error(yield, cancel, ec);
    }
}

}} // namespaces
