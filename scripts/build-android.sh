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
ALLOWED_MODES="bootstrap build publish"
DEFAULT_MODES="bootstrap build"

function check_mode {
    if echo "$MODES" | grep -q "\b$1\b"; then
        return 0
    fi
    return 1
}

######################################################################
function maybe_install_sdk {
    local toolsfile=commandlinetools-linux-13114758_latest.zip

    # Reuse downloaded SDK stuff from old versions of this script.
    if [ -d "$DIR/sdk_root" -a ! -d "$SDK_DIR" ]; then
        mv "$DIR/sdk_root" "$SDK_DIR"
    fi

    if [ ! -f "$SDK_MANAGER" ]; then
        echo "cannot find SDK manager: $SDK_MANAGER"
        echo "downlodaing SDK..."
        [ -d "$SDK_DIR/cmdline-tools" ] || rm -rf "$SDK_DIR/cmdline-tools"
        if [ ! -f "$toolsfile" ]; then
            # https://developer.android.com/studio/command-line/
            wget -nv "https://dl.google.com/android/repository/$toolsfile"
        fi
        unzip -q "$toolsfile" -d "$SDK_DIR"
    fi
}

function setup_sdk_deps {
    # SDK packages needed by the different modes.
    # To get list of all packages, use `sdkmanager --list`.
    local sdk_pkgs
    declare -A sdk_pkgs
    # Build dependencies are handled by Gradle once `$ANDROID_HOME` is set.
    sdk_pkgs[build]="
"

    # Collect SDK packages that need to be installed for
    # all the modes requested in the command line.
    local sdk_pkgs_install mode pkg
    for mode in $ALLOWED_MODES; do
        if check_mode $mode; then
            for pkg in ${sdk_pkgs[$mode]}; do
                if [ ! -d "$SDK_DIR/$(echo $pkg | tr ';' /)" ]; then
                    sdk_pkgs_install="$sdk_pkgs_install $pkg"
                fi
            done
        fi
    done

    # Filter out repeated packages.
    sdk_pkgs_install=$(echo "$sdk_pkgs_install" | tr [:space:] '\n' | sort -u)
    # Install missing packages.
    if [ "$sdk_pkgs_install" ]; then
        # This produces progress bars that are very frequently updated
        # and clutter build logs.
        yes y | "$SDK_MANAGER" --sdk_root=$SDK_DIR $sdk_pkgs_install > /dev/null
    fi
    # Accept licenses from stuff installed by Gradle on previous runs.
    # Not very clean, but if Gradle complains about licenses not being accepted,
    # at least a second run would succeed.
    yes y | "$SDK_MANAGER" --sdk_root=$SDK_DIR --licenses > /dev/null

    # Prefer locally installed platform tools to those in the system.
    export PATH="$SDK_DIR/platform-tools:$PATH"

    export ANDROID_HOME="$SDK_DIR"
}

######################################################################
function maybe_install_gradle {
    check_mode build || return 0

    GRADLE_REQUIRED_MAJOR_VERSION=8
    GRADLE_REQUIRED_MINOR_VERSION=14
    GRADLE_REQUIRED_PATCH_VERSION=3

    NEED_GRADLE=false

    if ! command -v gradle > /dev/null; then
       NEED_GRADLE=true
    else
        GRADLE_VERSION=`gradle -v | grep Gradle | cut -d ' ' -f 2`
        GRADLE_MAJOR_VERSION=`echo $GRADLE_VERSION | cut -d '.' -f1`
        GRADLE_MINOR_VERSION=`echo $GRADLE_VERSION | cut -d '.' -f2`
        GRADLE_PATCH_VERSION=`echo $GRADLE_VERSION | cut -d '.' -f3`

        if [ $GRADLE_REQUIRED_MAJOR_VERSION -gt $GRADLE_MAJOR_VERSION ]; then
            NEED_GRADLE=true
        else
            if [ $GRADLE_REQUIRED_MAJOR_VERSION -eq $GRADLE_MAJOR_VERSION ]; then
                 if [ $GRADLE_REQUIRED_MINOR_VERSION -gt $GRADLE_MINOR_VERSION ]; then
                    if [ $GRADLE_REQUIRED_PATCH_VERSION -gt $GRADLE_PATCH_VERSION ]; then
                        NEED_GRADLE=true
                    fi
                 fi
             fi
        fi
    fi

    echo need gradle? $NEED_GRADLE

    if [ $NEED_GRADLE == true ]; then
        local GRADLE=gradle-8.14.3
        local GRADLE_ZIP=$GRADLE-bin.zip
        if [ ! -d "$GRADLE" ]; then
            if [ ! -f $GRADLE_ZIP ]; then
                echo "downloading gradle..."
                wget -nv https://services.gradle.org/distributions/$GRADLE_ZIP
            fi
            #TODO: Check SHA256
            unzip -q $GRADLE_ZIP
        fi
        export PATH="`pwd`/$GRADLE/bin:$PATH"
    fi
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

if check_mode bootstrap; then
    maybe_install_sdk
    setup_sdk_deps
    maybe_install_gradle
    # TODO: miniupnp
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
