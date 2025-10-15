# ouinet-rs (ouinet components written in rust)

include(FetchContent)

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.2
)

if ("${PLATFORM}" STREQUAL "OS64")
    set(Rust_CARGO_TARGET aarch64-apple-ios)
elseif ("${PLATFORM}" STREQUAL "SIMULATORARM64")
    set(Rust_CARGO_TARGET aarch64-apple-ios-sim)
endif()

FetchContent_MakeAvailable(Corrosion)

set(OUINET_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

corrosion_import_crate(MANIFEST_PATH ${OUINET_ROOT_DIR}/rust/Cargo.toml)
corrosion_set_env_vars(ouinet_rs
    "CXXFLAGS=-I${CMAKE_CURRENT_BINARY_DIR}/boost/install/include -I${OUINET_ROOT_DIR}/rust")
add_dependencies(cargo-prebuild_ouinet_rs built_boost)

if ("${CMAKE_GENERATOR}" STREQUAL "Xcode")
    corrosion_set_env_vars(ouinet_rs
        "LIBRARY_PATH=${CMAKE_OSX_SYSROOT}/usr/lib")
    set(RUST_BRIDGE_PATH ${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}/cargo/build/${Rust_CARGO_TARGET_CACHED}/cxxbridge)
else()
    set(RUST_BRIDGE_PATH ${CMAKE_BINARY_DIR}/cargo/build/${Rust_CARGO_TARGET_CACHED}/cxxbridge)
endif()
# This is not necessry to build the ouinet_rs library, but using it
# automatically adds the include directories to every target that has ouinet_rs
# as dependency.
# XXX: Isn't there a better way to obtain the cargo target directory?
target_include_directories(ouinet_rs
    INTERFACE ${OUINET_ROOT_DIR}/rust
    ${RUST_BRIDGE_PATH})
