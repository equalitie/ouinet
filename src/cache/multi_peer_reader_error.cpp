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
                case Errc::expected_chunk_body: return "expected chunk body";
                case Errc::expected_chunk_hdr: return "expected chunk hdr";
                case Errc::no_peers: return "no peers to load from";
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
