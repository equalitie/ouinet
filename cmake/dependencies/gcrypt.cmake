include(ExternalProject)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    get_filename_component(COMPILER_TOOLCHAIN_PREFIX ${_CMAKE_TOOLCHAIN_PREFIX} NAME)
    string(REGEX REPLACE "-$" "" COMPILER_HOSTTRIPLE ${COMPILER_TOOLCHAIN_PREFIX})
    # This is the same as COMPILER_HOSTTRIPLE, _except_ on arm32.
    set(COMPILER_CC_PREFIX ${COMPILER_HOSTTRIPLE})

    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
        set(COMPILER_CC_PREFIX "armv7a-linux-androideabi")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-32.h")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-64.h")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-32.h")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(GPG_ERROR_CONFIG
            "${CMAKE_CURRENT_LIST_DIR}/gpg-error-config/lock-android-64.h")
    else()
        message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(GCRYPT_CC ${COMPILER_DIR}/${COMPILER_CC_PREFIX}${ANDROID_PLATFORM_LEVEL}-clang)
    # We need to supply an architecture/OS-specific config file,
    # and gpg-error does not supply it for most android builds.
    set(PATCH_COMMAND
        cp ${GPG_ERROR_CONFIG} ${CMAKE_CURRENT_BINARY_DIR}/gpg_error/src/gpg_error/src/syscfg/lock-obj-pub.linux-android.h
    )
    set(HOST_CONFIG "--host=${COMPILER_HOSTTRIPLE}")
    # For cross builds, gcrypt guesses an important toolchain characteristic
    # that it can't test for. Unfortunately, this guess is often wrong. This
    # value is right for android systems.
    set(UNDERSCORE_CONFIG "ac_cv_sys_symbol_underscore=no")
else()
    # TODO: Should probably support non-android cross compilation here.
    set(GCRYPT_CC ${CMAKE_C_COMPILER})
    set(PATCH_COMMAND "")
    set(HOST_CONFIG "")
    set(UNDERSCORE_CONFIG "")
endif()


if (CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(GCRYPT_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
else()
    set(GCRYPT_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

set(GPGERROR_FILENAME
    ${GCRYPT_OUTPUT_DIRECTORY}/${CMAKE_SHARED_LIBRARY_PREFIX}gpg-error${CMAKE_SHARED_LIBRARY_SUFFIX}
)
set(GPGERROR_BUILD_FILENAME
    "${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out/lib/${CMAKE_SHARED_LIBRARY_PREFIX}gpg-error${CMAKE_SHARED_LIBRARY_SUFFIX}"
)
set(GCRYPT_FILENAME
    ${GCRYPT_OUTPUT_DIRECTORY}/${CMAKE_SHARED_LIBRARY_PREFIX}gcrypt${CMAKE_SHARED_LIBRARY_SUFFIX}
)
set(GCRYPT_BUILD_FILENAME
    "${CMAKE_CURRENT_BINARY_DIR}/gcrypt/src/gcrypt/src/.libs/${CMAKE_SHARED_LIBRARY_PREFIX}gcrypt${CMAKE_SHARED_LIBRARY_SUFFIX}"
)

externalproject_add(gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.32.tar.bz2
    BUILD_IN_SOURCE 1
    PATCH_COMMAND
        "${PATCH_COMMAND}"
    CONFIGURE_COMMAND
        CC=${GCRYPT_CC}
            ./configure ${HOST_CONFIG}
            --prefix=${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out
    BUILD_COMMAND make
    BUILD_BYPRODUCTS ${GPGERROR_FILENAME}
    INSTALL_COMMAND
           make install
        && ${CMAKE_COMMAND} -E copy ${GPGERROR_BUILD_FILENAME} ${GCRYPT_OUTPUT_DIRECTORY}
    PREFIX gpg_error
)

externalproject_add(gcrypt
    DEPENDS gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.8.3.tar.bz2
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND
        CC=${GCRYPT_CC}
        ${UNDERSCORE_CONFIG}
            ./configure ${HOST_CONFIG}
            --with-libgpg-error-prefix=${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out
    BUILD_COMMAND make
    BUILD_BYPRODUCTS ${GCRYPT_FILENAME}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E copy ${GCRYPT_BUILD_FILENAME} ${GCRYPT_OUTPUT_DIRECTORY}
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
    INTERFACE ${GCRYPT_FILENAME}
)
