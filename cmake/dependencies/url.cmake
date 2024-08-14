include(ExternalProject)

# gcc 8 spits out warnings from Boost.Mpl about unnecessary parentheses
# https://github.com/CauldronDevelopmentLLC/cbang/issues/26
# (this library bundles Boost)
# TODO: Perhaps do a check for Boost and gcc version before adding this flag?
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-parentheses -Wno-error=nonnull -Wno-error=deprecated-declarations")

set(URL_FILENAME
    "${CMAKE_CURRENT_BINARY_DIR}/url/src/url-build/src/${CMAKE_STATIC_LIBRARY_PREFIX}skyr-url${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

externalproject_add(url
    GIT_REPOSITORY https://github.com/cpp-netlib/url
    GIT_TAG v1.13.0
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    CMAKE_ARGS
        -Dskyr_BUILD_TESTS=OFF
        -Dskyr_BUILD_DOCS=OFF
        -Dskyr_BUILD_EXAMPLES=OFF
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
    BUILD_BYPRODUCTS ${URL_FILENAME}
    PREFIX "url"
)

add_library(lib_url INTERFACE)
add_dependencies(lib_url url)
add_library(lib::url ALIAS lib_url)

target_include_directories(lib_url
    INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}/url/src/url/include"
)
target_link_libraries(lib_url
    INTERFACE ${URL_FILENAME}
)
