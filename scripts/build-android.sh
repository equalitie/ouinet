#!/bin/bash

set -e
# set -x

DIR=`pwd`
SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)
ABI=${ABI:-omni}

CMAKEARG_WITH_ASAN=${WITH_ASAN}
CMAKEARG_WITH_OUISYNC=${WITH_OUISYNC}
CMAKEARG_OUISYNC_SRC_DIR=${OUISYNC_SRC_DIR}

RELEASE_BUILD=0
while getopts r option; do
    case "$option" in
        r) RELEASE_BUILD=1;;
    esac
done
shift $((OPTIND -1))

# Please read `doc/android-sdk-versions.md` and keep in sync with it.
OUINET_TARGET_API=36
if [ "$ABI" = "armeabi-v7a" ]; then
    OUINET_MIN_API=26
elif [ "$ABI" = "arm64-v8a" ]; then
    OUINET_MIN_API=26
elif [ "$ABI" = "x86" ]; then
    OUINET_MIN_API=26
elif [ "$ABI" = "x86_64" ]; then
    OUINET_MIN_API=26
elif [ "$ABI" = "omni" ]; then
    OUINET_MIN_API=26
else
    >&2 echo "Unsupported ABI: '$ABI', valid values are armeabi-v7a, arm64-v8a, x86, x86_64."
    exit 1
fi

# Destination directory for Ouinet build outputs
OUTPUT_DIR=build-android-${ABI}
if [ $RELEASE_BUILD -eq 1 ]; then
    OUTPUT_DIR=${OUTPUT_DIR}-release
    GRADLE_VARIANT=Release
else
    OUTPUT_DIR=${OUTPUT_DIR}-debug
    GRADLE_VARIANT=Debug
fi
CMAKEARG_CMAKE_BUILD_TYPE=$GRADLE_VARIANT
mkdir -p "${DIR}/${OUTPUT_DIR}"
# Tasks to be run by Gradle when building the AAR.
# The explicit tasks are needed since there are no `build{Release,Debug}` tasks.
GRADLE_TASKS="${GRADLE_TASKS:-assemble${GRADLE_VARIANT} lint${GRADLE_VARIANT} test${GRADLE_VARIANT}UnitTest}"
PUBLISH_TASKS="${PUBLISH_TASKS:-publishToSonatype closeSonatypeStagingRepository}"

SDK_DIR=${SDK_DIR:-"$DIR/sdk"}
SDK_MANAGER="${SDK_DIR}/cmdline-tools/bin/sdkmanager"

cat <<EOF
Configuration environment variables:

ABI="$ABI"
SDK_DIR="$SDK_DIR"

EOF

# Export variables required by subprocesses (always prefixed with `OUINET_`).
export OUINET_MIN_API OUINET_TARGET_API

# Export extra arguments to be forwarded by Gradle to CMake (always prefixed
# with `CMAKEARG_`).
export CMAKEARG_CMAKE_BUILD_TYPE CMAKEARG_WITH_ASAN CMAKEARG_WITH_OUISYNC CMAKEARG_OUISYNC_SRC_DIR

######################################################################
MODES=
ALLOWED_MODES="build publish"
DEFAULT_MODES="build"

function check_mode {
    if echo "$MODES" | grep -q "\b$1\b"; then
        return 0
    fi
    return 1
}

######################################################################
# Build the Ouinet AAR
function build_ouinet_aar {
    GRADLE_BUILDDIR="${DIR}/${OUTPUT_DIR}/ouinet"
    OUINET_VERSION_NAME=$(cat "${ROOT}"/version.txt)
    OUINET_BUILD_ID=$(cd "${ROOT}" && "${ROOT}"/scripts/git-version-string.sh)
    mkdir -p "${GRADLE_BUILDDIR}"
    cp -r "${ROOT}"/android/gradle "${GRADLE_BUILDDIR}"/.
    ( cd "${GRADLE_BUILDDIR}";
      gradle $GRADLE_TASKS \
        -Pandroid_abi=${ABI} \
        -PversionName="${OUINET_VERSION_NAME}" \
        -PbuildId="${OUINET_BUILD_ID}" \
        -PbuildDir="${GRADLE_BUILDDIR}" \
        --project-dir="${ROOT}"/android \
        --gradle-user-home "${DIR}"/_gradle-home \
        --project-cache-dir "${GRADLE_BUILDDIR}"/_gradle-cache \
        --console plain \
        --no-daemon
    )
}

######################################################################
# Publish AAR artifacts to Maven Central
function publish_ouinet_aar {
    GRADLE_BUILDDIR="${DIR}/${OUTPUT_DIR}/ouinet"
    OUINET_VERSION_NAME=$(cat "${ROOT}"/version.txt)
    OUINET_BUILD_ID=$(cd "${ROOT}" && "${ROOT}"/scripts/git-version-string.sh)
    mkdir -p "${GRADLE_BUILDDIR}"
    ( cd "${GRADLE_BUILDDIR}";
      gradle $PUBLISH_TASKS \
        -Pandroid_abi=${ABI} \
        -PversionName="${OUINET_VERSION_NAME}" \
        -PbuildId="${OUINET_BUILD_ID}" \
        -PbuildDir="${GRADLE_BUILDDIR}" \
        --project-dir="${ROOT}"/android \
        --gradle-user-home "${DIR}"/_gradle-home \
        --project-cache-dir "${GRADLE_BUILDDIR}"/_gradle-cache \
        --console plain \
        --no-daemon
    )
}

######################################################################

# Parse modes

progname=$(basename "$0")
if [ "$1" = --help ]; then
    echo "Usage: $progname [MODE...]"
    echo "Accepted values of MODE: $ALLOWED_MODES"
    echo "If no MODE is provided, assume \"$DEFAULT_MODES\"."
    exit 0
fi

while [ -n "$1" -a "$1" ]; do
    if ! echo "$ALLOWED_MODES" | grep -q "\b$1\b"; then
        echo "$progname: unknown mode \"$1\"; accepted modes: $ALLOWED_MODES" >&2
        exit 1
    fi
    MODES="$MODES $1"
    shift
done

if [ ! "$MODES" ]; then
    MODES="$DEFAULT_MODES"
fi

if check_mode build; then
    build_ouinet_aar
fi

if check_mode publish; then
    if [[ "${GRADLE_VARIANT}" != "Release" ]]; then
      echo "Artifacts can be published only when the build is a Release."
      exit 1;
    else
      publish_ouinet_aar
    fi
fi
