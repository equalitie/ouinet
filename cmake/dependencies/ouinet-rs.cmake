# ouinet-rs (ouinet components written in rust)

include(FetchContent)

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.2
)

set(OUINET_RS_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(OUINET_RS_CXXFLAGS_LIST
    "-I${CMAKE_CURRENT_BINARY_DIR}/boost/install/include"
    "-I${OUINET_RS_ROOT_DIR}/rust"
)

if ("${PLATFORM}" STREQUAL "OS64")
    set(Rust_CARGO_TARGET aarch64-apple-ios)
elseif ("${PLATFORM}" STREQUAL "SIMULATORARM64")
    set(Rust_CARGO_TARGET aarch64-apple-ios-sim)
endif()

if (ANDROID)
    # The `__ANDROID_MIN_SDK_VERSION__` should be set by the compiler to the
    # same value as `__ANDROID_API__` [1], but for some reason this is not the
    # case. I don't know whether the problem is in Corrosion, in Cargo or in
    # the flags that are set when invoking cmake.
    #
    # During NDK header compilation, the precompiler code makes some decisions
    # sometimes based on the former and sometimes on the latter. Which causes
    # compilation errors with some functions not being defined.
    #
    # In particular, we found this issue with NDK's `pthread_cond_clockwait`
    # not being defined due to non presence of `__ANDROID_MIN_SDK_VERSION__`
    # but being used due to presence of `__ANDROID_API__`. This started
    # happening when we switched the NDK from 27.2.12479018 to 28.2.13676358.
    #
    # [1] https://developer.android.com/ndk/guides/sdk-versions#minsdkversion
    list(APPEND OUINET_RS_CXXFLAGS_LIST "-D__ANDROID_MIN_SDK_VERSION__=__ANDROID_API__")
endif()

FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH ${OUINET_RS_ROOT_DIR}/rust/Cargo.toml)

string(REPLACE ";" " " OUINET_RS_CXXFLAGS "${OUINET_RS_CXXFLAGS_LIST}")
corrosion_set_env_vars(ouinet_rs "CXXFLAGS=${OUINET_RS_CXXFLAGS}")

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
    INTERFACE ${OUINET_RS_ROOT_DIR}/rust
    ${RUST_BRIDGE_PATH})
