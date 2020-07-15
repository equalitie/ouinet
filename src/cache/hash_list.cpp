#include "hash_list.h"
#include "http_sign.h"
#include "chain_hasher.h"

using namespace std;
using namespace ouinet;
using namespace ouinet::cache;

#define _LOG_PFX "HashList: "
#define _WARN(...) LOG_WARN(_LOG_PFX, __VA_ARGS__)

static const size_t MAX_LINE_SIZE_BYTES = 512;

static const std::string MAGIC = "OUINET_HASH_LIST_V1";
static const char* ORIGINAL_STATUS = "X-Ouinet-Original-Status";

using Digest = util::SHA512::digest_type;

bool HashList::verify() const {
    size_t block_size = signed_head.block_size();

    ChainHasher chain_hasher;

    // Even responses with empty body have at least one block hash
    if (blocks.empty()) return false;

    ChainHash chain_hash;

    for (auto& block : blocks) {
        chain_hash = chain_hasher.calculate_block(
                block_size, block.data_hash, block.chained_hash_signature);
    }

    return chain_hash.verify( signed_head.public_key()
                            , signed_head.injection_id());
}

struct Parser {
    using Data = std::vector<uint8_t>;

    Data buffer;

    void append_data(const Data& data) {
        buffer.insert(buffer.end(), data.begin(), data.end());
    }

    // Returns a line of data (optionally)
    boost::optional<string> read_line() {
        auto nl_i = find_nl(buffer);

        if (nl_i == buffer.end()) {
            return boost::none;
        }

        string ret(buffer.begin(), nl_i);
        buffer.erase(buffer.begin(), std::next(nl_i));

        return ret;
    }

    boost::optional<util::Ed25519PublicKey::sig_array_t>
    read_signature() {
        return read_array<util::Ed25519PublicKey::sig_size>();
    }

    boost::optional<Digest>
    read_hash() {
        return read_array<util::SHA512::size()>();
    }

    template<size_t N>
    boost::optional<std::array<uint8_t, N>> read_array() {
        if (buffer.size() < N) return boost::none;
        auto b = buffer.begin();
        auto e = b + N;
        std::array<uint8_t, N> ret;
        std::copy(b, e, ret.begin());
        buffer.erase(b, e);
        return ret;
    }

    Data::iterator find_nl(Data& data) const {
        return std::find(data.begin(), data.end(), '\n');
    }
};

/* static */
HashList HashList::load(
        http_response::Reader& r,
        const PubKey& pk,
        Cancel& c,
        asio::yield_context y)
{
    static const auto bad_msg = sys::errc::make_error_code(sys::errc::bad_message);

    assert(!c);

    sys::error_code ec;

    auto part = r.async_read_part(c, y[ec]);

    if (!ec && !part) {
        assert(0);
        ec = c ? asio::error::operation_aborted
               : sys::errc::make_error_code(sys::errc::bad_message);
    }

    if (c) ec = asio::error::operation_aborted;
    if (ec) return or_throw<HashList>(y, ec);

    return_or_throw_on_error(y, c, ec, HashList{});

    if (!part->is_head()) return or_throw<HashList>(y, bad_msg);

    auto raw_head = move(*part->as_head());

    if (raw_head.result() == http::status::not_found) {
        return or_throw<HashList>(y, asio::error::not_found);
    }

    auto orig_status_sv = raw_head[ORIGINAL_STATUS];
    auto orig_status = parse::number<unsigned>(orig_status_sv);
    raw_head.erase(ORIGINAL_STATUS);

    if (!orig_status) {
        return or_throw<HashList>(y, bad_msg);
    }

    raw_head.result(*orig_status);

    auto head_o = SignedHead::verify_and_create(move(raw_head), pk);

    if (!head_o) return or_throw<HashList>(y, bad_msg);

    head_o->erase(http::field::content_length);
    head_o->set(http::field::transfer_encoding, "chunked");

    Parser parser;

    using Signature = PubKey::sig_array_t;

    bool magic_checked = false;

    boost::optional<Digest> digest;
    boost::optional<Signature> signature;

    std::vector<Block> blocks;

    while (true) {
        part = r.async_read_part(c, y[ec]);
        return_or_throw_on_error(y, c, ec, HashList{});

        if (!part) break;

        if (part->is_body()) {
            parser.append_data(*part->as_body());
        } else if (part->is_chunk_body()) {
            parser.append_data(*part->as_chunk_body());
        } else {
            continue;
        }

        while (true) {
            bool progress = false;

            if (!magic_checked) {
                auto magic_line = parser.read_line();
                if (magic_line) {
                    if (*magic_line != MAGIC)
                        return or_throw<HashList>(y, bad_msg);
                    magic_checked = true;
                    progress = true;
                }
            } else {
                if (!digest) {
                    digest = parser.read_hash();
                    if (digest) progress = true;
                } else {
                    assert(!signature);
                    signature = parser.read_signature();

                    if (signature) {
                        progress = true;

                        blocks.push_back({*digest, *signature});

                        digest    = boost::none;
                        signature = boost::none;
                    }
                }
            }

            if (!progress) {
                if (parser.buffer.size() > MAX_LINE_SIZE_BYTES) {
                    _WARN("Line too long");
                    return or_throw<HashList>(y, bad_msg);
                }
                break;
            }
        }
    }

    if (blocks.empty()) return or_throw<HashList>(y, bad_msg);

    HashList hs{move(*head_o), move(blocks)};

    if (!hs.verify()) {
        return or_throw<HashList>(y, bad_msg);
    }

    return hs;
}

void HashList::write(GenericStream& con, Cancel& c, asio::yield_context y) const
{
    using namespace chrono_literals;

    assert(verify());
    assert(!c);
    if (c) return or_throw(y, asio::error::operation_aborted);

    sys::error_code ec;

    auto h = signed_head;

    size_t content_length =
        MAGIC.size() + strlen("\n") +
        blocks.size() * (PubKey::sig_size + util::SHA512::size());

    h.set(ORIGINAL_STATUS, util::str(h.result_int()));
    h.result(http::status::ok);
    h.set(http::field::content_length, content_length);

    std::vector<asio::const_buffer> bufs;
    bufs.reserve(2 /* 2 = MAGIC + "\n" */ + blocks.size() * 2 /* 2 = signature + digest */);

    bufs.push_back(asio::buffer(MAGIC));
    bufs.push_back(asio::buffer("\n", 1));

    for (auto& block : blocks) {
        bufs.push_back(asio::buffer(block.data_hash));
        bufs.push_back(asio::buffer(block.chained_hash_signature));
    }

    auto wd = watch_dog(con.get_executor(),
            5s + 100ms * blocks.size(),
            [&] { con.close(); });

    h.async_write(con, c, y[ec]);
    if (!c && !wd.is_running()) ec = asio::error::timed_out;
    return_or_throw_on_error(y, c, ec);

    asio::async_write(con, bufs, y[ec]);

    if (!c && !wd.is_running()) ec = asio::error::timed_out;
    return_or_throw_on_error(y, c, ec);
}
