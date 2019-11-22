#pragma once

#include <exception>
#include <memory>

#include <boost/filesystem.hpp>

#include "../../logger.h"


namespace ouinet {

class CACertificate;
class EndCertificate;

namespace detail { namespace get_or_gen_tls_cert {

using path = boost::filesystem::path;

inline void log_load(const std::unique_ptr<CACertificate>&) {
    LOG_DEBUG("Loading existing CA certificate");
}

inline void log_load_fail( const std::unique_ptr<CACertificate>&
                         , const path& cp, const path& kp, const path& dp
                         , const std::exception& e) {
    LOG_ERROR( "Failed to load existing CA certificate: ", e.what()
             , "; cert=", cp, " key=", kp, " dh=", dp);
}

inline void log_gen(const std::unique_ptr<CACertificate>&) {
    LOG_DEBUG("Generating and storing CA certificate");
}

inline void log_gen_fail( const std::unique_ptr<CACertificate>&
                        , const path& cp, const path& kp, const path& dp
                        , const std::exception& e) {
    LOG_ERROR( "Failed to generate and store CA certificate: ", e.what()
             , "; cert=", cp, " key=", kp, " dh=", dp);
}

inline void log_load(const std::unique_ptr<EndCertificate>&) {
    LOG_DEBUG("Loading existing TLS end certificate");
}

inline void log_load_fail( const std::unique_ptr<EndCertificate>&
                         , const path& cp, const path& kp, const path& dp
                         , const std::exception& e) {
    LOG_ERROR( "Failed to load existing TLS end certificate: ", e.what()
             , "; cert=", cp, " key=", kp, " dh=", dp);
}

inline void log_gen(const std::unique_ptr<EndCertificate>&) {
    LOG_DEBUG("Generating and storing TLS end certificate");
}

inline void log_gen_fail( const std::unique_ptr<EndCertificate>&
                        , const path& cp, const path& kp, const path& dp
                        , const std::exception& e) {
    LOG_ERROR( "Failed to generate and store TLS end certificate: ", e.what()
             , "; cert=", cp, " key=", kp, " dh=", dp);
}

}}}  // namespaces
