#!/bin/bash

set -e
set -x

DIR=`pwd`

SCRIPT_DIR=$(dirname -- "$(readlink -f -- "${BASH_SOURCE[@]}")")
ROOT=$(cd "${SCRIPT_DIR}"/.. && pwd)
# Possible targets: MAC_ARM64, OS64, SIMULATORARM64, COMBINED
PLATFORM=${PLATFORM:-MAC_ARM64}

BUILD_TYPE='Debug'
while getopts r option; do
    case "$option" in
        r) BUILD_TYPE='Release';;
        *) echo "Default to debug build";;
    esac
done
shift $((OPTIND -1))

# Destination directory for Ouinet build outputs
OUTPUT_DIR=build-$(tr '[:upper:]' '[:lower:]' <<< "$PLATFORM")

IOS_TOOLCHAIN_PATH=${ROOT}/ios/toolchain/ios.toolchain.cmake

DEVELOPMENT_TEAM=5SR9R72Z83

MACOS_BUILD_ROOT=${DIR}/build-mac_arm64

BUNDLE_ID=ie.equalit.ouinet

BOOST_VERSION=1.88.0

CMAKE_CONFIG_ARGS="-GXcode \
-DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
-DCMAKE_TOOLCHAIN_FILE='$IOS_TOOLCHAIN_PATH' \
-DBOOST_VERSION=${BOOST_VERSION}"

CMAKE_BUILD_ARGS="-target ALL_BUILD \
-hideShellScriptEnvironment \
-allowProvisioningUpdates \
-parallelizeTargets \
-quiet \
ENABLE_BITCODE=0 \
PRODUCT_BUNDLE_IDENTIFIER=${BUNDLE_ID} \
DEVELOPMENT_TEAM=${DEVELOPMENT_TEAM}"

######################################################################
MODES=
ALLOWED_MODES="clean build"
DEFAULT_MODES="clean build"

function check_mode {
    if echo "$MODES" | grep -q "\b$1\b"; then
        return 0
    fi
    return 1
}

function clean {
    rm -rf ${OUTPUT_DIR}
}

function config {
    mkdir -p ${OUTPUT_DIR}
    pushd ${OUTPUT_DIR}
    cmake ${ROOT} ${CMAKE_CONFIG_ARGS} -DPLATFORM=${PLATFORM} -DMACOS_BUILD_ROOT="$MACOS_BUILD_ROOT"
    popd
}

function build {
    pushd ${OUTPUT_DIR}
    cmake --build . --config ${BUILD_TYPE} -- ${CMAKE_BUILD_ARGS}
    popd
}

function combine {
    if [[ -d ${DIR}/build-OS64 ]]; then
        if [[ ${DIR}/build-SIMULATORARM64 ]]; then
            
            # Create frameworks for both architectures
            add_framework_headers \
                "${ROOT}/ios/ouinet/include" \
                "${DIR}/build-os64/${BUILD_TYPE}-iphoneos/ouinet.framework" 

            add_framework_headers \
                "${ROOT}/ios/ouinet/include" \
                "${DIR}/build-simulatorarm64/${BUILD_TYPE}-iphonesimulator/ouinet.framework" 
            
            xcodebuild -create-xcframework \
                -framework ${DIR}/build-os64/${BUILD_TYPE}-iphoneos/ouinet.framework \
                -framework ${DIR}/build-simulatorarm64/${BUILD_TYPE}-iphonesimulator/ouinet.framework \
                -output ${DIR}/${OUTPUT_DIR}/ouinet.xcframework
        else
            echo "ERROR: ${DIR}/build-iphonesimulator not found, please build before combining frameworks"
            exit 1
        fi
    else
        echo "ERROR: ${DIR}/build-iphonesimulator not found, please build before combining frameworks"
        exit 1
    fi
}

function add_framework_headers {
    local HEADERS_PATH=$1
    local OUTPUT_PATH=$2
    local FRAMEWORK_NAME="Ouinet"

    mkdir -p "${OUTPUT_PATH}/Headers"
    mkdir -p "${OUTPUT_PATH}/Modules"
    
    # Copy headers
    cp -R "${HEADERS_PATH}/"* "${OUTPUT_PATH}/Headers/"
    

    # Create module.modulemap
    cat > "${OUTPUT_PATH}/Modules/module.modulemap" << EOF
framework module ${FRAMEWORK_NAME} {
  umbrella "Headers"
  export *
  module * { export * }
}
EOF
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
    clean
fi

if check_mode build; then
    if [ "$PLATFORM" == "COMBINED" ]; then
        combine
    else
        config
        build
    fi
    
fi
