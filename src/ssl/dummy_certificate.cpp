#include "dummy_certificate.h"
#include "ca_certificate.h"
#include "util.h"

using namespace std;
using namespace ouinet;

DummyCertificate::DummyCertificate( CACertificate& ca_cert
                                  , const string& cn)
    : _x(X509_new())
{
    X509_set_version(_x, ca_cert.x509_version);
    ASN1_INTEGER_set(X509_get_serialNumber(_x), ca_cert.next_serial_number());

    // Avoid signature issues because of time zone differences.
    // See [Mitmproxy can't record traffic when time set with 1 hour ago.](https://github.com/mitmproxy/mitmproxy/issues/200).
    X509_gmtime_adj(X509_get_notBefore(_x), -48 * ssl::util::ONE_HOUR);
    // A value close to maximum CA-emitted certificate validity (39 months), see
    // [Validity Period, 9.4.1](https://cabforum.org/wp-content/uploads/BRv1.2.3.pdf).
    // For iOS 13+, trusted certs must have validity period of 825 days or fewer
    // https://support.apple.com/en-us/103769
    X509_gmtime_adj(X509_get_notAfter(_x), 2 * ssl::util::ONE_YEAR);

    X509_set_pubkey(_x, ca_cert.get_private_key());
    
    string wc_cn("*." + cn);
    X509_NAME* name = X509_get_subject_name(_x); 

    if (!X509_NAME_add_entry_by_txt( name, "CN"
                                   , MBSTRING_ASC,
                                     (const unsigned char*) wc_cn.data()
                                     , wc_cn.size(), -1, 0))
        throw runtime_error("Failed in X509_NAME_add_entry_by_txt");
    
    if (!X509_set_issuer_name(_x, ca_cert.get_subject_name()))
        throw runtime_error("Failed in X509_set_issuer_name");

    string alt_name("DNS.1:*." + cn + ",DNS.2:" + cn);
    // Add various standard extensions
    ssl::util::x509_add_ext(_x, NID_subject_alt_name, alt_name.c_str());
    ssl::util::x509_add_ext(_x, NID_ext_key_usage, "serverAuth");

    if (!X509_sign(_x, ca_cert.get_private_key(), EVP_sha256()))
        throw runtime_error("Failed in X509_sign");

    {
        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(bio, _x);
        _pem_certificate = ssl::util::read_bio(bio);
        BIO_free_all(bio);
    }
}


DummyCertificate::~DummyCertificate()
{
    if (_x) X509_free(_x);
}

DummyCertificate::DummyCertificate(DummyCertificate&& other)
    : _x(other._x)
    , _pem_certificate(move(other._pem_certificate))
{
    other._x = nullptr;
}

DummyCertificate& DummyCertificate::operator=(DummyCertificate&& other)
{
    if (_x) X509_free(_x);

    _x = other._x;
    other._x = nullptr;
    _pem_certificate = move(other._pem_certificate);

    return *this;
}
