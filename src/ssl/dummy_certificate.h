#pragma once

#include <boost/beast/core/string.hpp>
#include <string>
#include <openssl/x509v3.h>

#include "../namespaces.h"

namespace ouinet {

class CACertificate;

class DummyCertificate {
public:
    // If `cn` is ``example.com``, this generates a certificate for
    // ``*.example.com`` with ``example.com`` as an alternative name.
    DummyCertificate(CACertificate&, const std::string& cn);

    DummyCertificate(const DummyCertificate&) = delete;
    DummyCertificate& operator=(const DummyCertificate&) = delete;

    DummyCertificate(DummyCertificate&&);
    DummyCertificate& operator=(DummyCertificate&&);

    const std::string& pem_certificate() const { return _pem_certificate; }

    ~DummyCertificate();

private:
    X509* _x;

    std::string _pem_certificate;
};

} // namespace
