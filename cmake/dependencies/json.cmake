include(ExternalProject)

externalproject_add(json
    # TODO: We only really need to download one header file.
    URL https://github.com/nlohmann/json/archive/v2.1.1.tar.gz
    BUILD_COMMAND ""
    UPDATE_COMMAND ""
    INSTALL_COMMAND ""
    PREFIX json
)

add_library(lib_json INTERFACE)
add_dependencies(lib_json json)
add_library(lib::json ALIAS lib_json)

target_include_directories(lib_json
    INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}/json/src/json/src"
)
