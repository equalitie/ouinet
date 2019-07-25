#!/bin/bash

set -e
set -x

DIR=`pwd`
SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)
ABI=${ABI:-armeabi-v7a}

if which apt-get 1> /dev/null 2>&1; then
    DEBIAN=true
else
    DEBIAN=false
fi

# Derive other variables from the selected ABI.
# See `$NDK/build/tools/make_standalone_toolchain.py:get_{triple,abis}()`.
# See <https://github.com/opencv/opencv/blob/5b868ccd829975da5372bf330994553e176aee09/platforms/android/android.toolchain.cmake#L658>.
if [ "$ABI" = "armeabi-v7a" ]; then
    NDK_ARCH="arm"
    NDK_TOOLCHAIN_TARGET="arm-linux-androideabi"
    NDK_TOOLCHAIN_LIB_SUBDIR="lib/armv7-a"
    NDK_PLATFORM=19
    CMAKE_SYSTEM_PROCESSOR="armv7-a"

elif [ "$ABI" = "arm64-v8a" ]; then
    NDK_ARCH="arm64"
    NDK_TOOLCHAIN_TARGET="aarch64-linux-android"
    NDK_PLATFORM=21
    CMAKE_SYSTEM_PROCESSOR="aarch64"

elif [ "$ABI" = "armeabi" ]; then
    NDK_ARCH="arm"
    NDK_TOOLCHAIN_TARGET="arm-linux-androideabi"
    NDK_PLATFORM=19
    CMAKE_SYSTEM_PROCESSOR="armv5te"

elif [ "$ABI" = "x86" ]; then
    NDK_ARCH="x86"
    NDK_TOOLCHAIN_TARGET="i686-linux-android"
    NDK_PLATFORM=19
    CMAKE_SYSTEM_PROCESSOR="i686"

elif [ "$ABI" = "x86_64" ]; then
    NDK_ARCH="x86_64"
    NDK_TOOLCHAIN_TARGET="x86_64-linux-android"
    NDK_TOOLCHAIN_LIB_SUBDIR="lib64"
    NDK_PLATFORM=21
    CMAKE_SYSTEM_PROCESSOR="x86_64"

else
    >&2 echo "Unsupported ABI: '$ABI'"
    exit 1
fi

# Destination directory for Ouinet build outputs
OUTPUT_DIR=build-android-${ABI}
mkdir -p "${DIR}/${OUTPUT_DIR}"
BUILD_DIR=build-ouinet-${ABI}
mkdir -p $BUILD_DIR

SDK_DIR=${SDK_DIR:-"$DIR/sdk"}

NDK=android-ndk-r16b
NDK_DIR=${NDK_DIR:-"$DIR/$NDK"}
NDK_ZIP=${NDK}-linux-x86_64.zip

NDK_STL='libc++'
NDK_TOOLCHAIN_DIR=${NDK_TOOLCHAIN_DIR:-${DIR}/${NDK}-toolchain-android$NDK_PLATFORM-$NDK_ARCH-$NDK_STL}

# Android API level, see https://redmine.equalit.ie/issues/12143
PLATFORM=android-${NDK_PLATFORM}

ANDROID_FLAGS="\
    -DBoost_COMPILER='-clang' \
    -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_SYSTEM_VERSION=${NDK_PLATFORM} \
    -DCMAKE_ANDROID_STANDALONE_TOOLCHAIN=${NDK_TOOLCHAIN_DIR} \
    -DCMAKE_SYSROOT=$NDK_TOOLCHAIN_DIR/sysroot \
    -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR} \
    -DCMAKE_ANDROID_ARCH_ABI=${ABI}"

EMULATOR_AVD=${EMULATOR_AVD:-ouinet-test}

# The following options may be limited by availability of SDK packages,
# to get list of all packages, use `sdkmanager --list`.

# The image to be used by the emulator AVD
EMULATOR_IMAGE_TAG=google_apis  # uses to be available for all platforms and ABIs
EMULATOR_IMAGE="system-images;$PLATFORM;$EMULATOR_IMAGE_TAG;$ABI"

# To get list of all devices, use `avdmanager list device`.
EMULATOR_DEV=${EMULATOR_DEV:-Nexus 6}
EMULATOR_SKIN=1440x2560  # automatically scaled down on smaller screens

echo "NDK_DIR: "$NDK_DIR
echo "SDK_DIR: "$SDK_DIR
echo "NDK_TOOLCHAIN_DIR: "$NDK_TOOLCHAIN_DIR
echo "NDK_PLATFORM: "$NDK_PLATFORM
echo "PLATFORM: "$PLATFORM

######################################################################
# This variable shall contain paths to generated libraries which
# must all be included in the final Android package.
OUT_LIBS=()

function add_library {
    local libs=("$@") lib
    for lib in "${libs[@]}"; do
        if [ ! -f "$lib" ]; then
            echo "Cannot add library \"$lib\": File doesn't exist"
            exit 1
        fi
        OUT_LIBS+=("$lib")
    done
}

######################################################################
# This variable shall contain paths to generated binaries which
# must all be included in the final Android package.
OUT_BINARIES=()

function add_binary {
    local binaries=("$@") binaries
    for binary in "${binaries[@]}"; do
        if [ ! -f "$binary" ]; then
            echo "Cannot add binary \"$binary\": File doesn't exist"
            exit 1
        fi
        OUT_BINARIES+=("$binary")
    done
}

######################################################################
MODES=
ALLOWED_MODES="bootstrap build emu"
DEFAULT_MODES="bootstrap build"

function check_mode {
    if echo "$MODES" | grep -q "\b$1\b"; then
        return 0
    fi
    return 1
}

######################################################################
function setup_deps {
    # J2EE is no longer part of standard Java modules in Java 9,
    # although the Android SDK uses some of its classes.
    # This causes exception "java.lang.NoClassDefFoundError: javax/xml/bind/...",
    # so we need to reenable J2EE modules explicitly when using JRE 9
    # (see <https://stackoverflow.com/a/43574427>).
    local java_add_modules=' --add-modules java.se.ee'
    if [ $DEBIAN == true ] ; then
        if [ $(java -version 2>&1 | awk -F[\"\.] -v OFS=. 'NR==1{print $3}') -ge 9 \
             -a "${JAVA_OPTS%%$java_add_modules*}" = "$JAVA_OPTS" ] ; then
            export JAVA_OPTS="$JAVA_OPTS$java_add_modules"
        fi
    fi

    ######################################################################
    # Install SDK dependencies.
    local toolsfile=sdk-tools-linux-3859397.zip
    local sdkmanager="$SDK_DIR/tools/bin/sdkmanager"

    # Reuse downloaded SDK stuff from old versions of this script.
    if [ -d "$DIR/sdk_root" -a ! -d "$SDK_DIR" ]; then
        mv "$DIR/sdk_root" "$SDK_DIR"
    fi

    if [ ! -f "$sdkmanager" ]; then
        echo "cannot find sdk manager: $sdkmangaer"
        echo "downlodaing sdk.."
        [ -d "$SDK_DIR/tools" ] || rm -rf "$SDK_DIR/tools"
        if [ ! -f "$toolsfile" ]; then
            # https://developer.android.com/studio/index.html#command-tools
            wget -nv "https://dl.google.com/android/repository/$toolsfile"
        fi
        unzip -q "$toolsfile" -d "$SDK_DIR"
    fi

    # SDK packages needed by the different modes.
    # To get list of all packages, use `sdkmanager --list`.
    local sdk_pkgs
    declare -A sdk_pkgs
    sdk_pkgs[build]="
platforms;$PLATFORM
build-tools;25.0.3
platform-tools
cmake;3.6.4111459
"
    sdk_pkgs[emu]="
$EMULATOR_IMAGE
platforms;$PLATFORM
platform-tools
emulator
"

    # Collect SDK packages that need to be installed for the requested modes.
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
        echo y | "$sdkmanager" $sdk_pkgs_install
    fi

    # Prefer locally installed platform tools to those in the system.
    export PATH="$SDK_DIR/platform-tools:$PATH"

    export ANDROID_HOME=$(dirname $(dirname $(which adb)))
}

######################################################################
# Create Android virtual device for the emulator.
function maybe_create_avd {
    if ! "$SDK_DIR/tools/emulator" -list-avds | grep -q "^$EMULATOR_AVD$"; then
        echo no | "$SDK_DIR/tools/bin/avdmanager" create avd -n "$EMULATOR_AVD" \
                                                  -k "$EMULATOR_IMAGE" -g "$EMULATOR_IMAGE_TAG" \
                                                  -b "$ABI" -d "$EMULATOR_DEV"
    fi
}

######################################################################
function maybe_install_ndk {
    if [ ! -d "$NDK_DIR" ]; then
        echo "Installing NDK..."
        if [ ! -f ${NDK_ZIP} ]; then
            wget -nv https://dl.google.com/android/repository/${NDK_ZIP}
        fi
        unzip -q ${NDK_ZIP}
    fi
}

######################################################################
function maybe_install_ndk_toolchain {
    if [ ! -d "${NDK_TOOLCHAIN_DIR}" ]; then
        echo "Installing NDK toolchain..."
        $NDK_DIR/build/tools/make-standalone-toolchain.sh \
            --platform=android-$NDK_PLATFORM \
            --arch=$NDK_ARCH \
            --stl=$NDK_STL \
            --install-dir=${NDK_TOOLCHAIN_DIR}
    fi

    export ANDROID_NDK_HOME=$NDK_DIR

    NDK_TOOLCHAIN_LIB_SUBDIR=${NDK_TOOLCHAIN_LIB_SUBDIR:-"lib"}
    TOOLCHAIN_LIBCXX="$NDK_TOOLCHAIN_DIR/$NDK_TOOLCHAIN_TARGET/$NDK_TOOLCHAIN_LIB_SUBDIR/libc++_shared.so"
    add_library $TOOLCHAIN_LIBCXX
    echo "TOOLCHAIN_LIBCXX: $TOOLCHAIN_LIBCXX"
}

######################################################################
function maybe_install_gradle {
    GRADLE_REQUIRED_MAJOR_VERSION=4
    GRADLE_REQUIRED_MINOR_VERSION=6

    NEED_GRADLE=false

    if ! which gradle 1> /dev/null 2>&1; then
       NEED_GRADLE=true
    else
        GRADLE_VERSION=`gradle -v | grep Gradle | cut -d ' ' -f 2`
        GRADLE_MAJOR_VERSION=`echo $GRADLE_VERSION | cut -d '.' -f1`
        GRADLE_MINOR_VERSION=`echo $GRADLE_VERSION | cut -d '.' -f2`

        if [ $GRADLE_REQUIRED_MAJOR_VERSION -gt $GRADLE_MAJOR_VERSION ]; then
            NEED_GRADLE=true
        else
            if [ $GRADLE_REQUIRED_MAJOR_VERSION -eq $GRADLE_MAJOR_VERSION ]; then
                 if [ $GRADLE_REQUIRED_MINOR_VERSION -gt $GRADLE_MINOR_VERSION ]; then
                     NEED_GRADLE=true
                 fi
             fi
        fi
    fi

    echo need gradle? $NEED_GRADLE

    if [ $NEED_GRADLE == true ]; then
        local GRADLE=gradle-4.6
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
# TODO: miniupnp
#   https://i2pd.readthedocs.io/en/latest/devs/building/android/

######################################################################
function build_ouinet_libs {
    BUILD_TYPE=Debug
    if [ $RELEASE_BUILD -eq 1 ]; then
        BUILD_TYPE=Release
    fi
    cd $BUILD_DIR
    cmake ${ANDROID_FLAGS} \
          -DWITH_INJECTOR=OFF \
          -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
          ${ROOT}
    make -j `nproc`
    cd - >/dev/null

    add_library $DIR/$BUILD_DIR/libclient.so
    add_library $DIR/$BUILD_DIR/libboost_asio.so
    add_library $DIR/$BUILD_DIR/libboost_asio_ssl.so
    add_library $DIR/$BUILD_DIR/modules/asio-ipfs/ipfs_bindings/libipfs_bindings.so
    add_library $DIR/$BUILD_DIR/gcrypt/src/gcrypt/src/.libs/libgcrypt.so
    add_library $DIR/$BUILD_DIR/gpg_error/out/lib/libgpg-error.so
    add_library $DIR/$BUILD_DIR/src/ouiservice/lampshade/lampshade_bindings/liblampshade_bindings.so
    add_binary  $DIR/$BUILD_DIR/src/ouiservice/obfs4proxy/obfs4proxy
}

######################################################################
function copy_jni_libs {
    local jni_dst_dir="${DIR}"/${OUTPUT_DIR}/builddir/deps/${ABI}
    rm -rf "${jni_dst_dir}"
    mkdir -p "${jni_dst_dir}"
    local lib
    for lib in "${OUT_LIBS[@]}"; do
        echo "Copying $lib to $jni_dst_dir"
        cp $lib $jni_dst_dir/
    done
}

######################################################################
function copy_binaries {
    local binary_dst_dir="${DIR}"/${OUTPUT_DIR}/builddir/assets/
    rm -rf "${binary_dst_dir}"
    mkdir -p "${binary_dst_dir}"
    local binary
    for binary in "${OUT_BINARIES[@]}"; do
        echo "Copying $binary to $binary_dst_dir"
        cp $binary $binary_dst_dir/
    done
}

######################################################################
# Build the Ouinet AAR
function build_ouinet_aar {
    export GRADLE_USER_HOME="${DIR}/.gradle-home"
    GRADLE_BUILDDIR="${DIR}/${OUTPUT_DIR}/ouinet"
    mkdir -p "${GRADLE_BUILDDIR}"
    ( cd "${ROOT}/android";
      gradle build \
        -Pboost_includedir="${DIR}/${BUILD_DIR}/boost/install/include" \
        -Pandroid_abi=${ABI} \
        -Pouinet_clientlib_path="${DIR}/${OUTPUT_DIR}/builddir/deps/${ABI}/libclient.so" \
        -Pasio_path="${DIR}/${OUTPUT_DIR}/builddir/deps/${ABI}/libboost_asio.so" \
        -Plibdir="${DIR}/${OUTPUT_DIR}/builddir/deps" \
        -Passetsdir="${DIR}/${OUTPUT_DIR}/builddir/assets" \
        -Pouinet_properties="${DIR}/${BUILD_DIR}/ouinet.properties" \
        -PbuildDir="$GRADLE_BUILDDIR" )
}

######################################################################
# Run the Android emulator with the AVD created above.
# The `-use-system-libs` option is necessary to avoid errors like
# "libGL error: unable to load driver" and X error `BadValue` on
# `X_GLXCreateNewContext`.
function run_emulator {
    echo "Starting Android emulator, first boot may take more than 10 minutes..."
    "$SDK_DIR/tools/emulator" -avd "$EMULATOR_AVD" -skin "$EMULATOR_SKIN" \
                              -use-system-libs "$@" &
    local emupid=$!
    # Inspired in <https://android.stackexchange.com/q/83726>.
    adb -e wait-for-device
    # Candidates:
    #   - sys.boot_completed == 1
    #   - service.bootanim.exit == 1
    #   - ro.runtime.firstboot != 0 (nor empty)
    adb -e shell 'while [ "$(getprop ro.runtime.firstboot 0)" -lt 1 ]; do sleep 1; done'
    cat << EOF

The emulated Android environment is now running.
Once you can interact with it normally, you may execute:

  - To install the APK: $(which adb) -e install $APK
  - To uninstall the APK: $(which adb) -e uninstall $APK_ID

EOF
    wait $emupid
}

######################################################################

# Parse modes and leave emulator arguments.

RELEASE_BUILD=0
while getopts r option; do
    case "$option" in
    r) RELEASE_BUILD=1;;
    esac
done
shift $((OPTIND -1))

progname=$(basename "$0")
if [ "$1" = --help ]; then
    echo "Usage: $progname [MODE...] [-- EMULATOR_ARG...]"
    echo "Accepted values of MODE: $ALLOWED_MODES"
    echo "If no MODE is provided, assume \"$DEFAULT_MODES\"."
    exit 0
fi

while [ -n "$1" -a "$1" != -- ]; do
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

if check_mode bootstrap; then
    setup_deps
    maybe_install_ndk
    maybe_install_ndk_toolchain
    maybe_install_gradle
    # TODO: miniupnp
fi

if check_mode build; then
    build_ouinet_libs
    copy_jni_libs
    copy_binaries
    build_ouinet_aar
fi

if check_mode emu; then
    maybe_create_avd
    run_emulator "$@"
fi

