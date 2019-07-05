#include "http_sign.h"

#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/beast/http/field.hpp>

#include "../util.h"
#include "../util/hash.h"

namespace ouinet { namespace cache {

std::string
http_digest(const http::response<http::dynamic_body>& rs)
{
    ouinet::util::SHA256 hash;

    // Feed each buffer of body data into the hash.
    for (auto it : rs.body().data())
        hash.update(it);
    auto digest = hash.close();
    auto encoded_digest = ouinet::util::base64_encode(digest);
    return "SHA-256=" + encoded_digest;
}

template<class Head>
static void
prep_sig_head(const Head& inh, Head& outh)
{
    using namespace std;

    // Lowercase header names, to more-or-less respect input order.
    vector<string> hdr_sorted;
    // Lowercase header name to `, `-concatenated, trimmed values.
    map<string, string> hdr_values;

    for (auto& hdr : inh) {
        auto name = hdr.name_string().to_string();  // lowercase
        boost::algorithm::to_lower(name);

        auto value_v = hdr.value();  // trimmed
        while (value_v.starts_with(' ')) value_v.remove_prefix(1);
        while (value_v.ends_with  (' ')) value_v.remove_suffix(1);

        auto vit = hdr_values.find(name);
        if (vit == hdr_values.end()) {  // new entry, add
            hdr_values[name] = value_v.to_string();
            hdr_sorted.push_back(name);
        } else {  // existing entry, concatenate
            vit->second += ", ";
            vit->second.append(value_v.data(), value_v.length());
        }
    }

    for (auto name : hdr_sorted)
        outh.set(name, hdr_values[name]);
}

template<class Head>
static std::pair<std::string, std::string>
get_sig_str_hdrs(const Head& sig_head)
{
    std::string sig_string, headers;
    bool ins_sep = false;
    for (auto& hdr : sig_head) {
        auto name = hdr.name_string();
        auto value = hdr.value();

        if (ins_sep) sig_string += '\n';
        sig_string += (boost::format("%s: %s") % name % value).str();

        if (ins_sep) headers += ' ';
        headers.append(name.data(), name.length());

        ins_sep = true;
    }

    return {sig_string, headers};
}

std::string
http_signature( const http::response_header<>& rsh
              , const ouinet::util::Ed25519PrivateKey& sk)
{
    // TODO: Compute proper signature string and sign it.
    auto fmt = boost::format("keyId=\"ed25519=%s\""
                             ",algorithm=\"hs2019\""
                             ",created=%d"
                             ",headers=\"%s\"");

    // TODO: Cache somewhere, doing this every time is quite inefficient.
    auto encoded_pk = ouinet::util::base64_encode(sk.public_key().serialize());
    auto ts = std::chrono::seconds(std::time(nullptr)).count();

    http::response_header<> sig_head;
    sig_head.set("(created)", ts);
    prep_sig_head(rsh, sig_head);  // unique fields, lowercase names, trimmed values

    std::string sig_string, headers;
    std::tie(sig_string, headers) = get_sig_str_hdrs(sig_head);

    return (fmt % encoded_pk % ts % headers).str();
}

}} // namespaces
