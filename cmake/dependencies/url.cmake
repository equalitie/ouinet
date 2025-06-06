include(ExternalProject)

set(URL_FILENAME
    "${CMAKE_CURRENT_BINARY_DIR}/url/src/url-build/src/${CMAKE_STATIC_LIBRARY_PREFIX}skyr-url${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

externalproject_add(expected
        URL https://github.com/TartanLlama/expected/archive/v1.1.0.tar.gz
        URL_HASH SHA256=1db357f46dd2b24447156aaf970c4c40a793ef12a8a9c2ad9e096d9801368df6
        UPDATE_COMMAND ""
        CMAKE_ARGS
            -DEXPECTED_BUILD_TESTS=OFF
            -DEXPECTED_BUILD_PACKAGE=OFF
            -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/expected/install
            -DCMAKE_INSTALL_DATADIR=${CMAKE_CURRENT_BINARY_DIR}/expected/install/share
        PREFIX expected
)

add_library(lib_expected INTERFACE)
add_dependencies(lib_expected expected)
add_library(lib::expected ALIAS lib_expected)

externalproject_add(range_v3
        URL https://github.com/ericniebler/range-v3/archive/0.12.0.tar.gz
        URL_HASH SHA256=015adb2300a98edfceaf0725beec3337f542af4915cec4d0b89fa0886f4ba9cb
        UPDATE_COMMAND ""
        CMAKE_ARGS
            -DRANGE_V3_DOCS=OFF
            -DRANGE_V3_TESTS=OFF
            -DRANGE_V3_EXAMPLES=OFF
            -DRANGE_V3_PERF=OFF
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/range_v3/install
        PREFIX range_v3
)

add_library(lib_range_v3 INTERFACE)
add_dependencies(lib_range_v3 range_v3)
add_library(lib::range_v3 ALIAS lib_range_v3)

externalproject_add(url
    DEPENDS
        expected
        range_v3
    GIT_REPOSITORY https://github.com/cpp-netlib/url
    GIT_TAG v1.13.0
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    CMAKE_ARGS
        -Dskyr_BUILD_TESTS=OFF
        -Dskyr_BUILD_DOCS=OFF
        -Dskyr_BUILD_EXAMPLES=OFF
        -Dskyr_WARNINGS_AS_ERRORS=OFF
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -Dnlohmann_json_DIR=${CMAKE_CURRENT_BINARY_DIR}/json/src/json-build
        -Drange-v3_DIR=${CMAKE_CURRENT_BINARY_DIR}/range_v3/install/lib/cmake/range-v3
        -Dtl-expected_DIR=${CMAKE_CURRENT_BINARY_DIR}/expected/install/share/cmake/tl-expected
    BUILD_BYPRODUCTS ${URL_FILENAME}
    PREFIX "url"
)

add_library(lib_url INTERFACE)
add_dependencies(lib_url url)
add_library(lib::url ALIAS lib_url)

target_include_directories(lib_url
    INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}/expected/install/include"
        "${CMAKE_CURRENT_BINARY_DIR}/range_v3/install/include"
        "${CMAKE_CURRENT_BINARY_DIR}/url/src/url/include"
)
target_link_libraries(lib_url
    INTERFACE ${URL_FILENAME}
)
