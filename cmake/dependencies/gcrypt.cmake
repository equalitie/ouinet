include(ExternalProject)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
        set(HOSTTRIPLE "arm-linux-androideabi")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-32.h")
    elseif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "^arm.*")
        # Is this still relevant? armv<7 seems to be obsolete
        # from android 4.4 onwards.
        set(HOSTTRIPLE "armv5te-linux-androideabi")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-32.h")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        set(HOSTTRIPLE "aarch64-linux-android")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-64.h")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
        set(HOSTTRIPLE "i686-linux-android")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-32.h")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(HOSTTRIPLE "x86_64-linux-android")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-64.h")
    else()
        message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    # We need to supply an architecture/OS-specific config file,
    # and gpg-error does not supply it for most android builds.
    set(PATCH_COMMAND
        cp ${GPG_ERROR_CONFIG} ${CMAKE_CURRENT_BINARY_DIR}/gpg_error/src/gpg_error/src/syscfg/lock-obj-pub.${HOSTTRIPLE}.h
    )
    set(HOST_CONFIG "--host=${HOSTTRIPLE}")
else()
    # TODO: Should probably support non-android cross compilation here.
    set(PATCH_COMMAND "")
    set(HOST_CONFIG "")
endif()

externalproject_add(gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.32.tar.bz2
    BUILD_IN_SOURCE 1
    PATCH_COMMAND
        "${PATCH_COMMAND}"
    CONFIGURE_COMMAND
        CC=${CMAKE_C_COMPILER}
            ./configure ${HOST_CONFIG}
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
            ./configure ${HOST_CONFIG}
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
