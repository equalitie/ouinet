#include "http_store.h"

#include <boost/beast/http/empty_body.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>

#include "../or_throw.h"
#include "../util/file_io.h"
#include "../util/variant.h"
#include "http_sign.h"

namespace ouinet { namespace cache {

static const fs::path head_fname = "head";
static const fs::path body_fname = "body";
static const fs::path sigs_fname = "sigs";

// TODO: Refactor with `http_sign.cpp`.
static
http_response::Head
without_framing(const http_response::Head& rsh)
{
    http::response<http::empty_body> rs(rsh);
    rs.chunked(false);  // easier with a whole response
    rs.erase(http::field::content_length);  // 0 anyway because of empty body
    rs.erase(http::field::trailer);
    return rs.base();
}

static
void
insert_trailer(const http_response::Trailer::value_type& th, http_response::Head& head)
{
    auto thn = th.name_string();
    auto thv = th.value();
    if (!boost::regex_match(thn.begin(), thn.end(), http_::response_signature_hdr_rx)) {
        head.insert(th.name(), thn, thv);
        return;
    }

    // Signature, look for redundant signatures in head.
    // It is redundant if it has the same `keyId` and not-longer `headers`
    // (simplified heuristic). <- TODO: check for strict subset of headers
    static const std::string keyid_rx_("\bkeyId=\"[^\"]*\"");
    static const boost::regex keyid_rx(keyid_rx_);
    boost::cmatch thk_match;
    boost::regex_match(thv.begin(), thv.end(), thk_match, keyid_rx);
    assert(!thk_match.empty());
    auto thk = thk_match[0];

    static const std::string headers_rx_("\bheaders=\"[^\"]*\"");
    static const boost::regex headers_rx(headers_rx_);
    boost::cmatch thh_match;
    boost::regex_match(thv.begin(), thv.end(), thh_match, headers_rx);
    assert(!thh_match.empty());
    auto thh = thh_match[0];

    bool insert = true;
    for (auto hit = head.begin(); hit != head.end();) {
        auto hn = hit->name_string();
        auto hv = hit->value();
        if (!boost::regex_match(hn.begin(), hn.end(), http_::response_signature_hdr_rx)) {
            ++hit;
            continue;
        }

        boost::cmatch hk_match;
        boost::regex_match(hv.begin(), hv.end(), hk_match, keyid_rx);
        assert(!hk_match.empty());
        auto hk = hk_match[0];

        if (thk != hk) {  // sig from different key
            ++hit;
            continue;
        }

        boost::cmatch hh_match;
        boost::regex_match(hv.begin(), hv.end(), hh_match, headers_rx);
        assert(!hh_match.empty());
        auto hh = hh_match[0];

        if (hh.length() > thh.length()) {  // inserted signature is redundant
            insert = false;  // do not insert
            ++hit;
            continue;  // there may be other redundant signatures
        }

        hit = head.erase(hit);  // found redundant signature, remove
    }

    if (insert)
        head.insert(th.name(), thn, thv);
}

class SplittedWriter {
public:
    SplittedWriter(const fs::path& dirp, const asio::executor& ex)
        : dirp(dirp), ex(ex) {}

private:
    const fs::path& dirp;
    const asio::executor& ex;

    http_response::Head head;  // for merging in the trailer later on
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
    async_write_part(http_response::Head h, Cancel cancel, asio::yield_context yield)
    {
        assert(!headf);

        // Dump the head without framing headers.
        head = without_framing(h);

        sys::error_code ec;
        auto hf = create_file(head_fname, cancel, ec);
        return_or_throw_on_error(yield, cancel, ec);
        headf = std::move(hf);
        head.async_write(*headf, cancel, yield);
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
    async_write_part(http_response::Trailer t, Cancel cancel, asio::yield_context yield)
    {
        assert(headf);

        if (t.cbegin() == t.cend()) return;

        // Extend the head with trailer headers and dump again.
        for (const auto& th : t)
            insert_trailer(th, head);

        sys::error_code ec;
        util::file_io::fseek(*headf, 0, ec);
        if (!ec) util::file_io::truncate(*headf, 0, ec);
        if (!ec) head.async_write(*headf, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
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
