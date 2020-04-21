#pragma once

#include "../util/hash.h"
#include "../util/crypto.h"
#include "../response_part.h"

namespace ouinet { namespace cache {

struct HashList {

    http_response::Head      signed_head;

    std::string              injection_id;
    std::vector<std::string> hashes;
    std::string              signature; 

    bool verify() const;
};

}}
