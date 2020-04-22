#pragma once

#include "../util/hash.h"
#include "../util/crypto.h"
#include "../response_part.h"
#include "signed_head.h"

namespace ouinet { namespace cache {

struct HashList {

    SignedHead               signed_head;
    std::string              injection_id;
    std::vector<std::string> hashes;
    std::string              signature; 

    bool verify() const;
};

}}
