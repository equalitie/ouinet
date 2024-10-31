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

DEVELOPMENT_TEAM=5SR9R72Z83

MACOS_BUILD_ROOT=${ROOT}/build-macos

MACOS_BUNDLE_ID=org.equalitie.ouinet-macos

BOOST_VERSION=1.79.0

CMAKE_CONFIG_ARGS="-GXcode \
-DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
-DBOOST_VERSION=${BOOST_VERSION}"

MACOS_CMAKE_BUILD_ARGS="PRODUCT_BUNDLE_IDENTIFIER=${MACOS_BUNDLE_ID} DEVELOPMENT_TEAM=${DEVELOPMENT_TEAM}"

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

function clean_macos {
    rm -rf "$MACOS_BUILD_ROOT"
}

function config_macos {
    mkdir -p "$MACOS_BUILD_ROOT"
    pushd "$MACOS_BUILD_ROOT"
    cmake .. ${CMAKE_CONFIG_ARGS} -DPLATFORM=MAC_ARM64
    popd
}

function build_macos {
    pushd "$MACOS_BUILD_ROOT"
    cmake --build . --config ${BUILD_TYPE} -- ${MACOS_CMAKE_BUILD_ARGS}
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
    clean_macos
fi

if check_mode build; then
      config_macos
      build_macos
fi
