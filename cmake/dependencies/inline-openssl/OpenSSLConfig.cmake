set(OPENSSL_INCLUDE_DIR ${BUILT_OPENSSL_INCLUDE_DIR})
set(OPENSSL_SSL_LIBRARY ${BUILT_OPENSSL_SSL_LIBRARY})
set(OPENSSL_CRYPTO_LIBRARY ${BUILT_OPENSSL_CRYPTO_LIBRARY})
set(OPENSSL_LIBRARIES ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY})
set(OPENSSL_VERSION ${BUILT_OPENSSL_VERSION})

file(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})

if (NOT TARGET OpenSSL::Crypto)
    add_library(openssl_crypto UNKNOWN IMPORTED)
    set_target_properties(openssl_crypto PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    )

    add_library(openssl_crypto_ INTERFACE)
    add_library(OpenSSL::Crypto ALIAS openssl_crypto_)
    set_target_properties(openssl_crypto_ PROPERTIES
        INTERFACE_LINK_LIBRARIES openssl_crypto
    )
    add_dependencies(openssl_crypto_ built_openssl openssl_crypto)
endif()

if (NOT TARGET OpenSSL::SSL)
    add_library(openssl_ssl UNKNOWN IMPORTED)
    set_target_properties(openssl_ssl PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    )

    add_library(openssl_ssl_ INTERFACE)
    add_library(OpenSSL::SSL ALIAS openssl_ssl_)
    set_target_properties(openssl_ssl_ PROPERTIES
        INTERFACE_LINK_LIBRARIES "openssl_ssl;OpenSSL::Crypto"
    )
    add_dependencies(openssl_ssl_ built_openssl openssl_ssl)
endif()
