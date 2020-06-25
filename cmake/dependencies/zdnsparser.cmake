include(ExternalProject)

set(ZDNSPARSER_FILENAME
    "${CMAKE_CURRENT_BINARY_DIR}/zdnsparser/src/zdnsparser-build/lib/${CMAKE_STATIC_LIBRARY_PREFIX}zdnsparser${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

set(PATCHES
    ${CMAKE_CURRENT_LIST_DIR}/zdnsparser/disable-tests.patch
)

set(PATCH_COMMAND
    cd ${CMAKE_CURRENT_BINARY_DIR}/zdnsparser/src/zdnsparser
)
foreach (patch ${PATCHES})
    set(PATCH_COMMAND ${PATCH_COMMAND} && patch -p1 -i ${patch})
endforeach()

externalproject_add(zdnsparser
    GIT_REPOSITORY https://github.com/packetzero/dnsparser
    GIT_TAG 5ad991cd40dde95a3289e56fd7f65543f1967c67
    PATCH_COMMAND "${PATCH_COMMAND}"
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
    BUILD_BYPRODUCTS ${ZDNSPARSER_FILENAME}
    PREFIX "zdnsparser"
)

add_library(lib_zdnsparser INTERFACE)
add_dependencies(lib_zdnsparser zdnsparser)
add_library(lib::zdnsparser ALIAS lib_zdnsparser)

target_include_directories(lib_zdnsparser
    INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}/zdnsparser/src/zdnsparser/include"
)
target_link_libraries(lib_zdnsparser
    INTERFACE ${ZDNSPARSER_FILENAME}
)
