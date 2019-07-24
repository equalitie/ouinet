include(ExternalProject)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    set(GCRYPT_HOST "--host=${GCRYPT_TARGET}")
endif()

externalproject_add(gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.32.tar.bz2
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND
        CC=${CMAKE_C_COMPILER}
            ./configure ${GCRYPT_HOST}
            --prefix=${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out
    BUILD_COMMAND $(MAKE)
    PREFIX gpg_error
)

externalproject_add(gcrypt
    DEPENDS gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.8.3.tar.bz2
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND
        CC=${CMAKE_C_COMPILER}
            ./configure ${GCRYPT_HOST}
            --with-libgpg-error-prefix=${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    PREFIX gcrypt
)

add_library(lib_gcrypt INTERFACE)
add_dependencies(lib_gcrypt gcrypt)
add_library(lib::gcrypt ALIAS lib_gcrypt)

target_include_directories(lib_gcrypt
    INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}/gcrypt/src/gcrypt/src"
        "${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out/include"
)
target_link_libraries(lib_gcrypt
    INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}/gcrypt/src/gcrypt/src/.libs/${CMAKE_SHARED_LIBRARY_PREFIX}gcrypt${CMAKE_SHARED_LIBRARY_SUFFIX}"
)
