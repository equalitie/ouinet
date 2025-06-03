if(NOT BOOST_VERSION)
    set(BOOST_VERSION 1.87.0)
endif ()

if (${BOOST_VERSION} EQUAL 1.79.0)
    set(BOOST_VERSION_HASH 475d589d51a7f8b3ba2ba4eda022b170e562ca3b760ee922c146b6c65856ef39)
    set(BOOST_COROUTINE_BACKEND coroutine)
elseif (${BOOST_VERSION} EQUAL 1.87.0)
    set(BOOST_VERSION_HASH af57be25cb4c4f4b413ed692fe378affb4352ea50fbe294a11ef548f4d527d89)
    set(BOOST_COROUTINE_BACKEND fiber)
    if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
        # There is a bug in boost::outcome (used by cpp-upnp) which causes
        # compilation issues. This works around it but also disables some nicer
        # GDB error messages. I'm not sure it matters much on Android, plus
        # AFAIK we always check the `outcome::result` type before we access
        # it's value, so probably will also never happen.
        # Github issue for the bug is here: https://github.com/ned14/outcome/pull/308
        add_compile_definitions(BOOST_OUTCOME_SYSTEM_ERROR2_DISABLE_INLINE_GDB_PRETTY_PRINTERS=1)
        add_compile_definitions(BOOST_OUTCOME_DISABLE_INLINE_GDB_PRETTY_PRINTERS=1)
    endif()
    list(APPEND BOOST_PATCHES ${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-android-1_87_0.patch)
    list(APPEND BOOST_PATCHES ${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-windows-iocp-1_87_0.patch)
endif ()

set(BOOST_COMPONENTS
    context
    ${BOOST_COROUTINE_BACKEND}
    date_time
    filesystem
    iostreams
    nowide
    program_options
    regex
    system
    unit_test_framework
)

string(REPLACE "." "_" BOOST_VERSION_FILENAME ${BOOST_VERSION})

set(OUINET_BOOST_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/boost")
set(OUINET_BOOST_CONFIGURE_COMMAND ./bootstrap.sh)

if(BOOST_VERSION LESS_EQUAL 1.72.0)
    list(APPEND BOOST_PATCHES
        ${CMAKE_CURRENT_LIST_DIR}/inline-boost/beast-header-parser-fix-${BOOST_VERSION_FILENAME}.patch
        ${CMAKE_CURRENT_LIST_DIR}/inline-boost/thread-pthread-stack-min-def-${BOOST_VERSION_FILENAME}.patch
    )
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    # This is the same as CMAKE_LIBRARY_ARCHITECTURE, _except_ on arm32.
    set(COMPILER_CC_PREFIX ${CMAKE_LIBRARY_ARCHITECTURE})

    if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
        set(COMPILER_CC_PREFIX "armv7a-linux-androideabi")
        set(BOOST_ARCH "armeabiv7a")
        set(BOOST_ABI "aapcs")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
        set(BOOST_ARCH "arm64v8a")
        set(BOOST_ABI "aapcs")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
        set(BOOST_ARCH "x86")
        set(BOOST_ABI "sysv")
    elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(BOOST_ARCH "x8664")
        set(BOOST_ABI "sysv")
    else()
        message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(BOOST_VERSION LESS_EQUAL 1.77.0)
        list(APPEND BOOST_PATCHES ${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-android-${BOOST_VERSION_FILENAME}.patch)
    endif()

    set(BOOST_ENVIRONMENT
        export
            PATH=${COMPILER_DIR}:$ENV{PATH}
            BOOSTARCH=${BOOST_ARCH}
            # Before using Gradle 8 BINUTILS_PREFIX was set to "${COMPILER_DIR}/${COMPILER_HOSTTRIPLE}-"
            BINUTILS_PREFIX="${COMPILER_DIR}/llvm-"
            COMPILER_FULL_PATH=${COMPILER_DIR}/${COMPILER_CC_PREFIX}${ANDROID_PLATFORM_LEVEL}-clang++
        &&
    )
    set(BOOST_ARCH_CONFIGURATION
        --user-config=${CMAKE_CURRENT_LIST_DIR}/inline-boost/user-config-android.jam
        target-os=android
        toolset=clang-${BOOST_ARCH}
        abi=${BOOST_ABI}
    )
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(BOOST_ARCH_CONFIGURATION
        address-model=64
    )

    # TODO: Use this on all platforms
    if (CMAKE_BUILD_TYPE)
        string(TOLOWER "${CMAKE_BUILD_TYPE}" BOOST_BUILD_TYPE)
        list(APPEND BOOST_ARCH_CONFIGURATION variant=${BOOST_BUILD_TYPE})
    else()
        message(WARNING "No CMAKE_BUILD_TYPE provided, add '-DCMAKE_BUILD_TYPE={Debug,Release}' to cmake configuration phase")
    endif()

    if (MINGW)
        set(OUINET_BOOST_USER_CONFIG_FILE ${OUINET_BOOST_PREFIX}/user-config-mingw.jam)
        list(PREPEND OUINET_BOOST_CONFIGURE_COMMAND
            echo "using gcc : mingw : ${CMAKE_CXX_COMPILER} $<SEMICOLON>" > ${OUINET_BOOST_USER_CONFIG_FILE} &&)

        list(APPEND BOOST_ARCH_CONFIGURATION
            target-os=windows
            --user-config=${OUINET_BOOST_USER_CONFIG_FILE}
            toolset=gcc-mingw
            # https://lists.preview.boost.org/archives/list/boost@lists.preview.boost.org/thread/UEMVOSYBMS3MBQ77K3JGKOYRXSMSYLYC/
            define=_WIN32_WINNT=0x0601
            binary-format=pe
            abi=ms
        )
    endif()

    link_libraries(ws2_32 mswsock)
else()
    set(BOOST_ENVIRONMENT )
    set(BOOST_ARCH_CONFIGURATION )
endif()

set(BUILT_BOOST_VERSION ${BOOST_VERSION})
set(BUILT_BOOST_INCLUDE_DIR ${OUINET_BOOST_PREFIX}/install/include)
set(BUILT_BOOST_LIBRARY_DIR ${OUINET_BOOST_PREFIX}/install/lib)
set(BUILT_BOOST_COMPONENTS ${BOOST_COMPONENTS})

function(_boost_library_filename component output_var)
    set(${output_var} "${BUILT_BOOST_LIBRARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}boost_${component}${CMAKE_STATIC_LIBRARY_SUFFIX}" PARENT_SCOPE)
endfunction(_boost_library_filename)

include(${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-dependencies.cmake)
_static_Boost_recursive_dependencies("${BOOST_COMPONENTS}" BOOST_DEPENDENT_COMPONENTS)
set(ENABLE_BOOST_COMPONENTS )
set(BOOST_LIBRARY_FILES )
foreach (component ${BOOST_DEPENDENT_COMPONENTS})
    if (${component} STREQUAL "unit_test_framework")
        set(ENABLE_BOOST_COMPONENTS ${ENABLE_BOOST_COMPONENTS} --with-test)
        continue()
    endif()
    set(ENABLE_BOOST_COMPONENTS ${ENABLE_BOOST_COMPONENTS} --with-${component})
    _boost_library_filename(${component} filename)
    set(BOOST_LIBRARY_FILES ${BOOST_LIBRARY_FILES} ${filename})
endforeach()

set(BOOST_PATCH_COMMAND
    cd ${OUINET_BOOST_PREFIX}/src/built_boost
)
foreach (patch ${BOOST_PATCHES})
    set(BOOST_PATCH_COMMAND ${BOOST_PATCH_COMMAND} && patch -N -p1 -i ${patch})
endforeach()

execute_process(COMMAND nproc OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE NPROC)

externalproject_add(built_boost
    URL "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_FILENAME}.tar.bz2"
    URL_HASH SHA256=${BOOST_VERSION_HASH}
    PREFIX ${OUINET_BOOST_PREFIX}
    BUILD_IN_SOURCE 1
    PATCH_COMMAND ${BOOST_PATCH_COMMAND}
    CONFIGURE_COMMAND ${OUINET_BOOST_CONFIGURE_COMMAND}
    BUILD_COMMAND
        ${BOOST_ENVIRONMENT} ./b2
            link=static
            threading=multi
            -j${NPROC}
            #--verbose -d2 # Output exact commands when building
            --layout=system
            --prefix=${OUINET_BOOST_PREFIX}/install
            --no-cmake-config
            -q # Stop at first error
            ${ENABLE_BOOST_COMPONENTS}
            ${BOOST_ARCH_CONFIGURATION}
            install
    BUILD_BYPRODUCTS ${BOOST_LIBRARY_FILES}
    INSTALL_COMMAND ""
)

set(Boost_DIR ${CMAKE_CURRENT_LIST_DIR}/inline-boost)
list(INSERT CMAKE_MODULE_PATH 0 ${Boost_DIR})


find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS ${BOOST_COMPONENTS})
find_package(Threads REQUIRED)


if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # Boost.Beast has a patch that uses the ANDROID definition
    # instead of the implicit __ANDROID__ one.
    # https://github.com/boostorg/beast/blob/44f37d1a11d/include/boost/beast/core/impl/file_posix.ipp#L27
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "ANDROID"
    )
endif()


# gcc 8 spits out warnings from Boost.Mpl about unnecessary parentheses
# https://github.com/CauldronDevelopmentLLC/cbang/issues/26
# TODO: Perhaps do a check for Boost and gcc version before adding this flag?
set_target_properties(Boost::boost PROPERTIES
    INTERFACE_COMPILE_OPTIONS "-Wno-parentheses"
)


# Asio can be used as a header-only library _only_ if it is not used along any
# shared library boundaries. In any program using asio across a library
# boundary, both the library and the program must link asio as a shared library
# instead. Boost does not ship this library, so we need to create it.

add_library(boost_asio SHARED "${CMAKE_CURRENT_SOURCE_DIR}/lib/asio.cpp")
add_library(Boost::asio ALIAS boost_asio)
target_link_libraries(boost_asio
    PUBLIC
        Boost::boost
        Threads::Threads
        Boost::${BOOST_COROUTINE_BACKEND}
    PRIVATE
        Boost::system
)
if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows" AND BOOST_VERSION GREATER_EQUAL 1.77.0)
    # explicitly link with bcrypt after Boost::filesystem
    target_link_libraries(boost_asio
        PUBLIC
            crypt32
            bcrypt)
endif()
target_compile_definitions(boost_asio
    PUBLIC
        -DBOOST_ASIO_DYN_LINK
)
target_compile_options(boost_asio
    PUBLIC -std=c++20
)

add_library(boost_asio_ssl SHARED "${CMAKE_CURRENT_SOURCE_DIR}/lib/asio_ssl.cpp")
add_library(Boost::asio_ssl ALIAS boost_asio_ssl)
target_link_libraries(boost_asio_ssl
    PUBLIC
        OpenSSL::SSL
        Boost::asio
)


# FindBoost.cmake doesn't define targets for newer versions of boost.
# Let's emulate it instead.
foreach(component ${BOOST_COMPONENTS})
    if (NOT TARGET Boost::${component})
        include(${CMAKE_CURRENT_LIST_DIR}/inline-boost/boost-dependencies.cmake)
        _static_Boost_recursive_dependencies(${component} dependencies)

        find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS ${dependencies})
        list(GET Boost_LIBRARIES 0 imported_location)

        add_library(Boost::${component} UNKNOWN IMPORTED)
        set_target_properties(Boost::${component} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${Boost_LIBRARIES}"
            IMPORTED_LOCATION "${imported_location}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
        )
    endif()
endforeach()
