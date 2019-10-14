include(ExternalProject)

# gcc 8 spits out warnings from Boost.Mpl about unnecessary parentheses
# https://github.com/CauldronDevelopmentLLC/cbang/issues/26
# (this library bundles Boost)
# TODO: Perhaps do a check for Boost and gcc version before adding this flag?
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-parentheses")

set(URI_FILENAME
    "${CMAKE_CURRENT_BINARY_DIR}/uri/src/uri-build/src/${CMAKE_STATIC_LIBRARY_PREFIX}network-uri${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

externalproject_add(uri
    GIT_REPOSITORY https://github.com/cpp-netlib/uri
    GIT_TAG 1.0.1
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    CMAKE_ARGS
        -DUri_BUILD_TESTS=OFF
        -DUri_BUILD_DOCS=OFF
        -DUri_DISABLE_LIBCXX=""
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
    BUILD_BYPRODUCTS ${URI_FILENAME}
    PREFIX "uri"
)

add_library(lib_uri INTERFACE)
add_dependencies(lib_uri uri)
add_library(lib::uri ALIAS lib_uri)

target_include_directories(lib_uri
    INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}/uri/src/uri/include"
)
target_link_libraries(lib_uri
    INTERFACE ${URI_FILENAME}
)
