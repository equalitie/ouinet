set(OPENSSL_INCLUDE_DIR ${BUILT_OPENSSL_INCLUDE_DIR})
set(OPENSSL_SSL_LIBRARY ${BUILT_OPENSSL_SSL_LIBRARY})
set(OPENSSL_CRYPTO_LIBRARY ${BUILT_OPENSSL_CRYPTO_LIBRARY})
set(OPENSSL_LIBRARIES ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY})
set(OPENSSL_VERSION ${BUILT_OPENSSL_VERSION})

file(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})

if (NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    )
    add_dependencies(OpenSSL::Crypto built_openssl)
    # Make sure that any packages that link against the raw library file still
    # get the dependency order correct.
    add_custom_command(OUTPUT ${OPENSSL_CRYPTO_LIBRARY} COMMAND "" DEPENDS built_openssl)
endif()

if (NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL UNKNOWN IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    )
    add_dependencies(OpenSSL::SSL built_openssl)
    add_custom_command(OUTPUT ${OPENSSL_SSL_LIBRARY} COMMAND "" DEPENDS built_openssl)
endif()
