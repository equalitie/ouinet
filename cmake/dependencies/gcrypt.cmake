include(ExternalProject)

set(GPGERROR_LIBRARY_BASE_FILENAME
    ${CMAKE_SHARED_LIBRARY_PREFIX}gpg-error${CMAKE_SHARED_LIBRARY_SUFFIX}
)
set(GCRYPT_LIBRARY_BASE_FILENAME
    ${CMAKE_SHARED_LIBRARY_PREFIX}gcrypt${CMAKE_SHARED_LIBRARY_SUFFIX}
)

# The order of these lists is important.
# The first entry is a regular file, the remainder are symlinks.
set(GPGERROR_LIBRARY_VERSION_FILENAMES
    ${GPGERROR_LIBRARY_BASE_FILENAME}.0.38.0
    ${GPGERROR_LIBRARY_BASE_FILENAME}.0
    ${GPGERROR_LIBRARY_BASE_FILENAME}
)
set(GCRYPT_LIBRARY_VERSION_FILENAMES
    ${GCRYPT_LIBRARY_BASE_FILENAME}.20.5.0
    ${GCRYPT_LIBRARY_BASE_FILENAME}.20
    ${GCRYPT_LIBRARY_BASE_FILENAME}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(COMPILER_CC_PREFIX ${CMAKE_LIBRARY_ARCHITECTURE})

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
    set(HOST_CONFIG "--host=${CMAKE_LIBRARY_ARCHITECTURE}")
    # For cross builds, gcrypt guesses an important toolchain characteristic
    # that it can't test for. Unfortunately, this guess is often wrong. This
    # value is right for android systems.
    set(UNDERSCORE_CONFIG "ac_cv_sys_symbol_underscore=no")
    set(VERSIONED_LIBRARIES 0)
else()
    # TODO: Should probably support non-android cross compilation here.
    set(GCRYPT_CC ${CMAKE_C_COMPILER})
    set(PATCH_COMMAND "true")
    set(HOST_CONFIG "")
    set(UNDERSCORE_CONFIG "")
    set(VERSIONED_LIBRARIES 1)
endif()

set(PATCH_COMMAND
    ${PATCH_COMMAND} && cd ${CMAKE_CURRENT_BINARY_DIR}/gpg_error/src/gpg_error
)

if (CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(GCRYPT_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
else()
    set(GCRYPT_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

set(GPGERROR_BUILD_DIRECTORY
    ${CMAKE_CURRENT_BINARY_DIR}/gpg_error/out
)
set(GCRYPT_BUILD_DIRECTORY
    ${CMAKE_CURRENT_BINARY_DIR}/gcrypt/out
)



# The procedure for installing built libraries into the target directory is
# quite different for versions in which libraries are versioned
# (libgcrypt.so.20.2.3) such as linux-gnu, versus where they are not (android).
if (${VERSIONED_LIBRARIES})
    list(GET GPGERROR_LIBRARY_VERSION_FILENAMES 0 primary)
    list(REMOVE_AT GPGERROR_LIBRARY_VERSION_FILENAMES 0)

    set(GPGERROR_BYPRODUCTS ${GCRYPT_OUTPUT_DIRECTORY}/${primary})
    set(GPGERROR_INSTALL
        ${CMAKE_COMMAND} -E copy ${GPGERROR_BUILD_DIRECTORY}/lib/${primary} ${GCRYPT_OUTPUT_DIRECTORY}
    )
    foreach (filename ${GPGERROR_LIBRARY_VERSION_FILENAMES})
        set(GPGERROR_BYPRODUCTS ${GPGERROR_BYPRODUCTS}
            ${GCRYPT_OUTPUT_DIRECTORY}/${filename}
        )
        set(GPGERROR_INSTALL ${GPGERROR_INSTALL}
            && ${CMAKE_COMMAND} -E create_symlink ${primary} ${GCRYPT_OUTPUT_DIRECTORY}/${filename}
        )
    endforeach()

    list(GET GCRYPT_LIBRARY_VERSION_FILENAMES 0 primary)
    list(REMOVE_AT GCRYPT_LIBRARY_VERSION_FILENAMES 0)

    set(GCRYPT_BYPRODUCTS ${GCRYPT_OUTPUT_DIRECTORY}/${primary})
    set(GCRYPT_INSTALL
        ${CMAKE_COMMAND} -E copy ${GCRYPT_BUILD_DIRECTORY}/lib/${primary} ${GCRYPT_OUTPUT_DIRECTORY}
    )
    foreach (filename ${GCRYPT_LIBRARY_VERSION_FILENAMES})
        set(GCRYPT_BYPRODUCTS ${GCRYPT_BYPRODUCTS}
            ${GCRYPT_OUTPUT_DIRECTORY}/${filename}
        )
        set(GCRYPT_INSTALL ${GCRYPT_INSTALL}
            && ${CMAKE_COMMAND} -E create_symlink ${primary} ${GCRYPT_OUTPUT_DIRECTORY}/${filename}
        )
    endforeach()
else()
    set(GPGERROR_BYPRODUCTS ${GCRYPT_OUTPUT_DIRECTORY}/${GPGERROR_LIBRARY_BASE_FILENAME})
    set(GCRYPT_BYPRODUCTS ${GCRYPT_OUTPUT_DIRECTORY}/${GCRYPT_LIBRARY_BASE_FILENAME})

    set(GPGERROR_INSTALL
        ${CMAKE_COMMAND} -E copy ${GPGERROR_BUILD_DIRECTORY}/lib/${GPGERROR_LIBRARY_BASE_FILENAME} ${GCRYPT_OUTPUT_DIRECTORY}
    )
    set(GCRYPT_INSTALL
        ${CMAKE_COMMAND} -E copy ${GCRYPT_BUILD_DIRECTORY}/lib/${GCRYPT_LIBRARY_BASE_FILENAME} ${GCRYPT_OUTPUT_DIRECTORY}
    )
endif()



externalproject_add(gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-1.51.tar.bz2
    URL_HASH SHA256=be0f1b2db6b93eed55369cdf79f19f72750c8c7c39fc20b577e724545427e6b2
    PATCH_COMMAND
        "${PATCH_COMMAND}"
    CONFIGURE_COMMAND
        CC=${GCRYPT_CC}
            ./configure ${HOST_CONFIG}
            --prefix=${GPGERROR_BUILD_DIRECTORY}
            --enable-install-gpg-error-config
    BUILD_COMMAND make
    BUILD_IN_SOURCE 1
    BUILD_BYPRODUCTS ${GPGERROR_BYPRODUCTS}
    INSTALL_COMMAND
           make install
        && ${GPGERROR_INSTALL}
    PREFIX gpg_error
)

externalproject_add(gcrypt
    DEPENDS gpg_error
    URL https://www.gnupg.org/ftp/gcrypt/libgcrypt/libgcrypt-1.11.0.tar.bz2
    URL_HASH SHA256=09120c9867ce7f2081d6aaa1775386b98c2f2f246135761aae47d81f58685b9c
    CONFIGURE_COMMAND
        CC=${GCRYPT_CC}
        GPGRT_CONFIG=${GPGERROR_BUILD_DIRECTORY}/bin/gpgrt-config
        ${UNDERSCORE_CONFIG}
            ./configure ${HOST_CONFIG}
            --prefix=${GCRYPT_BUILD_DIRECTORY}
            --with-libgpg-error-prefix=${GPGERROR_BUILD_DIRECTORY}
            --disable-doc
    BUILD_COMMAND make
    BUILD_IN_SOURCE 1
    BUILD_BYPRODUCTS ${GCRYPT_BYPRODUCTS}
    INSTALL_COMMAND
           make install
        && ${GCRYPT_INSTALL}
    PREFIX gcrypt
)

add_library(lib_gcrypt INTERFACE)
add_dependencies(lib_gcrypt gcrypt)
add_library(lib::gcrypt ALIAS lib_gcrypt)

target_include_directories(lib_gcrypt
    INTERFACE
        ${GPGERROR_BUILD_DIRECTORY}/include
        ${GCRYPT_BUILD_DIRECTORY}/include
)
target_link_libraries(lib_gcrypt
    INTERFACE ${GCRYPT_OUTPUT_DIRECTORY}/${GCRYPT_LIBRARY_BASE_FILENAME}
)
