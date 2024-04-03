#!/bin/bash

set -e
set -x

SCRIPT_DIR=$(dirname -- "$(readlink -f -- "${BASH_SOURCE[@]}")")
ROOT=$(cd "${SCRIPT_DIR}"/.. && pwd)

BUILD_TYPE='Debug'
while getopts r option; do
    case "$option" in
        r) BUILD_TYPE='Release';;
        *) echo "Default to debug build";;
    esac
done
shift $((OPTIND -1))

IOS_TOOLCHAIN_PATH=${ROOT}/ios/ouinet/toolchain/ios.toolchain.cmake

DEVELOPMENT_TEAM=5SR9R72Z83

MACOS_BUILD_ROOT=${ROOT}/build-macos
IOS_BUILD_ROOT=${ROOT}/build-ios

MACOS_BUNDLE_ID=org.equalitie.ouinet-macos
IOS_BUNDLE_ID=org.equalitie.ouinet-ios

MACOS_CMAKE_BUILD_ARGS="PRODUCT_BUNDLE_IDENTIFIER=${MACOS_BUNDLE_ID} DEVELOPMENT_TEAM=${DEVELOPMENT_TEAM}"

IOS_XCODE_BUILD_ARGS="-target ALL_BUILD \
-configuration ${BUILD_TYPE} \
-hideShellScriptEnvironment \
-parallelizeTargets \
-allowProvisioningUpdates \
ENABLE_BITCODE=0 \
PRODUCT_BUNDLE_IDENTIFIER=${IOS_BUNDLE_ID} \
DEVELOPMENT_TEAM=${DEVELOPMENT_TEAM}"

######################################################################
MODES=
ALLOWED_MODES="clean build all mac ios sim"
DEFAULT_MODES="clean build all"

function check_mode {
    if echo "$MODES" | grep -q "\b$1\b"; then
        return 0
    fi
    return 1
}

function clean_macos {
    rm -rf "$MACOS_BUILD_ROOT"
}

function clean_ios {
    rm -rf "$IOS_BUILD_ROOT"
}

function config_macos {
    mkdir -p "$MACOS_BUILD_ROOT"
    pushd "$MACOS_BUILD_ROOT"
    cmake .. -GXcode -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_TOOLCHAIN_FILE="$IOS_TOOLCHAIN_PATH" -DPLATFORM=MAC_ARM64
    popd
}

function build_macos {
    pushd "$MACOS_BUILD_ROOT"
    cmake --build . --config ${BUILD_TYPE} -- ${MACOS_CMAKE_BUILD_ARGS}
    popd
}

function config_ios {
    mkdir -p "$IOS_BUILD_ROOT"
    pushd "$IOS_BUILD_ROOT"
    cmake .. -GXcode -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_TOOLCHAIN_FILE="$IOS_TOOLCHAIN_PATH" -DPLATFORM=OS64 -DMACOS_BUILD_ROOT="$MACOS_BUILD_ROOT"
    popd
}

function build_ios {
    pushd "$IOS_BUILD_ROOT"
    xcodebuild -project ouinet-iOS.xcodeproj build ${IOS_XCODE_BUILD_ARGS}
    echo "Build output: ${IOS_BUILD_ROOT}/${BUILD_TYPE}-iphoneos/ouinet-ios.framework"
    popd
}

######################################################################

# Parse modes and leave emulator arguments.

progname=$(basename "$0")
if [ "$1" = --help ]; then
    echo "Usage: $progname [MODE...] [-- EMULATOR_ARG...]"
    echo "Accepted values of MODE: $ALLOWED_MODES"
    echo "If no MODE is provided, assume \"$DEFAULT_MODES\"."
    exit 0
fi

while [ -n "$1" ] && [ "$1" != -- ]; do
    if ! echo "$ALLOWED_MODES" | grep -q "\b$1\b"; then
        echo "$progname: unknown mode \"$1\"; accepted modes: $ALLOWED_MODES" >&2
        exit 1
    fi
    MODES="$MODES $1"
    shift
done

if [ "$1" = -- ]; then
    shift  # leave the rest of arguments for the emulator
fi

if [ ! "$MODES" ]; then
    MODES="$DEFAULT_MODES"
fi

if check_mode clean; then
    if check_mode all; then
        clean_macos
        clean_ios
    fi
    if check_mode mac; then
        clean_macos
    fi
    if check_mode ios; then
        clean_ios
    fi
fi

if check_mode build; then
    if check_mode all; then
        config_macos
        build_macos
        config_ios
        build_ios
    fi
    if check_mode mac; then
        config_macos
        build_macos
    fi
    if check_mode ios; then
        config_ios
        build_ios
    fi
fi
