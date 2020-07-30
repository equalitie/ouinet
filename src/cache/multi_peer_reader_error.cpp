#include "multi_peer_reader_error.h"

namespace ouinet { namespace cache {
    
namespace {

    class MultiPeerReaderErrorCategoryImpl : public boost::system::error_category
    {
    public:
        const char* name() const noexcept override final { return "MultiPeerReader"; }
        
        std::string message(int e_) const {
            using Errc = MultiPeerReaderErrc;
    
            // Convert to enum to make compiler check we use all the codes in the
            // switch below.
            auto e = static_cast<Errc>(e_);
    
            switch (e) {
                case Errc::inconsistent_hash: return "inconsistent hash";
                case Errc::expected_head: return "expected head part";
                case Errc::expected_first_chunk_hdr: return "expected first chunk hdr";
                case Errc::expected_chunk_body: return "expected chunk body";
                case Errc::block_is_too_big: return "block is too big";
                case Errc::expected_chunk_hdr: return "expected chunk hdr";
                case Errc::no_peers: return "no peers to load from";
                case Errc::expected_trailer_or_end_of_response: return "expected trailer or end of respnse";
                case Errc::trailer_received_twice: return "trailer received twice";
                case Errc::expected_no_more_data: return "expected no more data";
            }
    
            return "unknown error";
        }
    };
} // anonymous namespace

boost::system::error_code make_error_code(MultiPeerReaderErrc e)
{
    static const MultiPeerReaderErrorCategoryImpl cat;
    return {static_cast<int>(e), cat};
}

}} // namespaces
