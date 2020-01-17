#define BOOST_TEST_MODULE http_store
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <string>

#include <cache/http_sign.h>

#include <namespaces.h>

BOOST_AUTO_TEST_SUITE(ouinet_http_store)

using namespace std;
using namespace ouinet;

// This signed response used below comes from `test-http-sign`.

static const string _rs_head_origin = (
    "HTTP/1.1 200 OK\r\n"
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Server: Apache2\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"
);

static const string _rs_head_injection = (
    "X-Ouinet-Version: 3\r\n"
    "X-Ouinet-URI: https://example.com/foo\r\n"
    "X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310\r\n"
    "X-Ouinet-BSigs: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",size=65536\r\n"
);

static const string _rs_head_sig0 = (
    "X-Ouinet-Sig0: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048310,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs\","
    "signature=\"tnVAAW/8FJs2PRgtUEwUYzMxBBlZpd7Lx3iucAt9q5hYXuY5ci9T7nEn7UxyKMGA1ZvnDMDBbs40dO1OQUkdCA==\"\r\n"
);

static const string _rs_head_framing = (
    "Transfer-Encoding: chunked\r\n"
    "Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1\r\n"
);

static const string rs_head =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_sig0
    + _rs_head_framing
    + "\r\n");

static const string _rs_head_digest = (
    "X-Ouinet-Data-Size: 131076\r\n"
    "Digest: SHA-256=E4RswXyAONCaILm5T/ZezbHI87EKvKIdxURKxiVHwKE=\r\n"
);

static const string _rs_head_sig1 = (
    "X-Ouinet-Sig1: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048311,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs "
    "x-ouinet-data-size "
    "digest\","
    "signature=\"h/PmOlFvScNzDAUvV7tLNjoA0A39OL67/9wbfrzqEY7j47IYVe1ipXuhhCfTnPeCyXBKiMlc4BP+nf0VmYzoAw==\"\r\n"
);

static const string rs_trailer =
    ( _rs_head_digest
    + _rs_head_sig1
    + "\r\n");

static const string _rs_block0_head = "0123";
static const string _rs_block0_tail = "4567";
static const string _rs_block1_head = "89AB";
static const string _rs_block1_tail = "CDEF";
static const string _rs_block2 = "abcd";
static const char _rs_block_fill_char = 'x';
static const size_t _rs_block_fill = ( http_::response_data_block
                                     - _rs_block0_head.size()
                                     - _rs_block0_tail.size());

static const array<string, 3> rs_block_data{
    _rs_block0_head + string(_rs_block_fill, _rs_block_fill_char) + _rs_block0_tail,
    _rs_block1_head + string(_rs_block_fill, _rs_block_fill_char) + _rs_block1_tail,
    _rs_block2,
};

static const array<string, 3> rs_block_hash{
    "aERfr5o+kpvR4ZH7xC0mBJ4QjqPUELDzjmzt14WmntxH2p3EQmATZODXMPoFiXaZL6KNI50Ve4WJf/x3ma4ieA==",
    "slwciqMQBddB71VWqpba+MpP9tBiyTE/XFmO5I1oiVJy3iFniKRkksbP78hCEWOM6tH31TGEFWP1loa4pqrLww==",
    "vyUR6T034qN7qDZO5vUILMP9FsJYPys1KIELlGDFCSqSFI7ZowrT3U9ffwsQAZSCLJvKQhT+GhtO0aM2jNnm5A==",
};

static const array<string, 3> rs_block_sig{
    "AwiYuUjLYh/jZz9d0/ev6dpoWqjU/sUWUmGL36/D9tI30oaqFgQGgcbVCyBtl0a7x4saCmxRHC4JW7cYEPWwCw==",
    "c+ZJUJI/kc81q8sLMhwe813Zdc+VPa4DejdVkO5ZhdIPPojbZnRt8OMyFMEiQtHYHXrZIK2+pKj2AO03j70TBA==",
    "m6sz1NpU/8iF6KNN6drY+Yk361GiW0lfa0aaX5TH0GGW/L5GsHyg8ozA0ejm29a+aTjp/qIoI1VrEVj1XG/gDA==",
};

BOOST_AUTO_TEST_SUITE_END()
