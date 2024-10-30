include(ExternalProject)

externalproject_add(json
    # TODO: We only really need to download one header file.
    # (https://github.com/nlohmann/json/releases/download/v3.6.1/json.hpp)
    URL https://github.com/nlohmann/json/archive/v3.11.3.tar.gz
    URL_MD5 d603041cbc6051edbaa02ebb82cf0aa9
    CONFIGURE_COMMAND ""
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
        "${CMAKE_CURRENT_BINARY_DIR}/json/src/json/single_include"
)
