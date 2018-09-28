#!/bin/bash

set -e

DIR=`pwd`
SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)
APP_ROOT="${ROOT}/android/browser"
APK="${DIR}"/build-android/builddir/browser/build-android/outputs/apk/debug/browser-debug.apk
APK_ID=$(sed -En 's/^\s*\bapplicationId\s+"([^"]+)".*/\1/p' "${APP_ROOT}/build.gradle")

# https://developer.android.com/ndk/guides/abis.html
ABI=${ABI:-armeabi-v7a}

# Derive other variables from the selected ABI.
# See `$NDK/build/tools/make_standalone_toolchain.py:get_{triple,abis}()`.
# See <https://github.com/opencv/opencv/blob/5b868ccd829975da5372bf330994553e176aee09/platforms/android/android.toolchain.cmake#L658>.
# See `$OPENSSL/config`.
if [ "$ABI" = "armeabi-v7a" ]; then
    NDK_ARCH="arm"
    NDK_TOOLCHAIN_TARGET="arm-linux-androideabi"
    CMAKE_SYSTEM_PROCESSOR="armv7-a"
    OPENSSL_MACHINE="armv7"
elif [ "$ABI" = "arm64-v8a" ]; then
    NDK_ARCH="arm64"
    NDK_TOOLCHAIN_TARGET="aarch64-linux-android"
    CMAKE_SYSTEM_PROCESSOR="aarch64"
    OPENSSL_MACHINE="arm64"
elif [ "$ABI" = "armeabi" ]; then
    NDK_ARCH="arm"
    NDK_TOOLCHAIN_TARGET="arm-linux-androideabi"
    CMAKE_SYSTEM_PROCESSOR="armv5te"
    OPENSSL_MACHINE="armv4"
elif [ "$ABI" = "x86" ]; then
    NDK_ARCH="x86"
    NDK_TOOLCHAIN_TARGET="i686-linux-android"
    CMAKE_SYSTEM_PROCESSOR="i686"
    OPENSSL_MACHINE="i686"
elif [ "$ABI" = "x86_64" ]; then
    NDK_ARCH="x86_64"
    NDK_TOOLCHAIN_TARGET="x86_64-linux-android"
    CMAKE_SYSTEM_PROCESSOR="x86_64"
    OPENSSL_MACHINE="x86_64"
else
    >&2 echo "TODO: Need a mapping from \"$ABI\" to other target selection variables"
    exit 1
fi

SDK_DIR="$DIR/sdk"

NDK=android-ndk-r16b
NDK_DIR=$DIR/$NDK
NDK_ZIP=${NDK}-linux-x86_64.zip

# `posix_fadvise`, required by Boost.Beast is was only added in LOLLIPOP
# https://developer.android.com/guide/topics/manifest/uses-sdk-element.html#ApiLevels
NDK_PLATFORM=21
NDK_STL='libc++'
NDK_TOOLCHAIN_DIR=${DIR}/${NDK}-toolchain-android$NDK_PLATFORM-$NDK_ARCH-$NDK_STL

BOOST_V=1_67_0
BOOST_V_DOT=${BOOST_V//_/.} # Replace '_' for '.'
BOOST_SOURCE=${DIR}/Boost-for-Android
BOOST_INCLUDEDIR=$BOOST_SOURCE/build/out/${ABI}/include
BOOST_LIBRARYDIR=$BOOST_SOURCE/build/out/${ABI}/lib

SSL_V="1.1.0g"
SSL_DIR=${DIR}/openssl-${SSL_V}

ANDROID_FLAGS="\
    -DBoost_COMPILER='-clang' \
    -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_SYSTEM_VERSION=${NDK_PLATFORM} \
    -DCMAKE_ANDROID_STANDALONE_TOOLCHAIN=${NDK_TOOLCHAIN_DIR} \
    -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR} \
    -DCMAKE_ANDROID_ARCH_ABI=${ABI} \
    -DOPENSSL_ROOT_DIR=${SSL_DIR} \
    -DBOOST_INCLUDEDIR=${BOOST_INCLUDEDIR} \
    -DBOOST_LIBRARYDIR=${BOOST_LIBRARYDIR}"

EMULATOR_AVD=${EMULATOR_AVD:-ouinet-test}

# The following options may be limited by availability of SDK packages,
# to get list of all packages, use `sdkmanager --list`.

# Android API level
PLATFORM=${PLATFORM:-android-25}

# The image to be used by the emulator AVD
EMULATOR_IMAGE_TAG=google_apis  # uses to be available for all platforms and ABIs
EMULATOR_IMAGE="system-images;$PLATFORM;$EMULATOR_IMAGE_TAG;$ABI"

# To get list of all devices, use `avdmanager list device`.
EMULATOR_DEV=${EMULATOR_DEV:-Nexus 6}
EMULATOR_SKIN=1440x2560  # automatically scaled down on smaller screens

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
MODES=
ALLOWED_MODES="build emu abiclean"
DEFAULT_MODES="build"

function check_mode {
    if echo "$MODES" | grep -q "\b$1\b"; then
        return 0
    fi
    return 1
}

######################################################################
function setup_deps {
which unzip > /dev/null || sudo apt-get install unzip
which java > /dev/null || sudo apt-get install default-jre
dpkg-query -W default-jdk > /dev/null 2>&1 || sudo apt-get install default-jdk

# J2EE is no longer part of standard Java modules in Java 9,
# although the Android SDK uses some of its classes.
# This causes exception "java.lang.NoClassDefFoundError: javax/xml/bind/...",
# so we need to reenable J2EE modules explicitly when using JRE 9
# (see <https://stackoverflow.com/a/43574427>).
local java_add_modules=' --add-modules java.se.ee'
if [ $(dpkg-query -W default-jre | cut -f2 | sed -En 's/^[0-9]+:1\.([0-9]+).*/\1/p') -ge 9 \
     -a "${JAVA_OPTS%%$java_add_modules*}" = "$JAVA_OPTS" ] ; then
    export JAVA_OPTS="$JAVA_OPTS$java_add_modules"
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
    [ -d "$SDK_DIR/tools" ] || rm -rf "$SDK_DIR/tools"
    if [ ! -f "$toolsfile" ]; then
        # https://developer.android.com/studio/index.html#command-tools
        wget "https://dl.google.com/android/repository/$toolsfile"
    fi
    unzip "$toolsfile" -d "$SDK_DIR"
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
if [ ! -d "$NDK" ]; then
    if [ ! -f ${NDK_ZIP} ]; then
        wget https://dl.google.com/android/repository/${NDK_ZIP}
    fi
    unzip ${NDK_ZIP}
fi
}

######################################################################
function maybe_install_ndk_toolchain {
if [ ! -d "${NDK_TOOLCHAIN_DIR}" ]; then
    $NDK_DIR/build/tools/make-standalone-toolchain.sh \
        --platform=android-$NDK_PLATFORM \
        --arch=$NDK_ARCH \
        --stl=$NDK_STL \
        --install-dir=${NDK_TOOLCHAIN_DIR}
fi

export ANDROID_NDK_HOME=$NDK_DIR

add_library $NDK_TOOLCHAIN_DIR/$NDK_TOOLCHAIN_TARGET/lib*/libc++_shared.so
}

######################################################################
function maybe_install_gradle {
local GRADLE=gradle-4.6
local GRADLE_ZIP=$GRADLE-bin.zip
if [ ! -d "$GRADLE" ]; then
    if [ ! -f $GRADLE_ZIP ]; then
        wget https://services.gradle.org/distributions/$GRADLE_ZIP
    fi
    # TODO: Check SHA256
    unzip $GRADLE_ZIP
fi

export PATH="`pwd`/$GRADLE/bin:$PATH"
}

######################################################################
function maybe_install_boost {

BOOST_GIT=https://github.com/equalitie/Boost-for-Android

if [ ! -d "$BOOST_SOURCE" ]; then
    (cd "$(dirname "$BOOST_SOURCE")" \
         && git clone $BOOST_GIT "$(basename "$BOOST_SOURCE")")
fi

if [ ! -d "$BOOST_LIBRARYDIR" ]; then
    cd "$BOOST_SOURCE"
    # TODO: Android doesn't need program_options and test.
    ./build-android.sh \
        --boost=${BOOST_V_DOT} \
        --arch=${ABI} \
        --with-libraries=regex,context,coroutine,program_options,system,test,thread,filesystem,date_time,iostreams \
        --layout=system \
        $NDK_DIR
    cd -
fi
}

######################################################################
function build_openssl {
    export SYSTEM=android
    export CROSS_SYSROOT="$NDK_TOOLCHAIN_DIR/sysroot"
    export ANDROID_DEV="$SYSROOT/usr"
    export MACHINE="$OPENSSL_MACHINE"
    export CC=gcc
    export CROSS_COMPILE="$NDK_TOOLCHAIN_TARGET-"
    export TOOLCHAIN="$NDK_TOOLCHAIN_DIR"
    export PATH="$NDK_TOOLCHAIN_DIR/bin:$PATH"
    ./config -v no-shared -no-ssl2 -no-ssl3 -no-comp -no-hw -no-engine
    make depend
    make build_libs
}

function maybe_install_openssl {
if [ ! -d "$SSL_DIR" ]; then
    if [ ! -f openssl-${SSL_V}.tar.gz ]; then
        wget https://www.openssl.org/source/openssl-${SSL_V}.tar.gz
    fi
    tar xf openssl-${SSL_V}.tar.gz
    (cd $SSL_DIR && build_openssl)
fi
}

######################################################################
function maybe_clone_ifaddrs {
if [ ! -d "android-ifaddrs" ]; then
    # TODO: Still need to compile the .c file and make use of it.
    git clone https://github.com/PurpleI2P/android-ifaddrs.git
fi
}

######################################################################
# TODO: miniupnp
#   https://i2pd.readthedocs.io/en/latest/devs/building/android/

######################################################################
function build_ouinet_libs {
mkdir -p build-ouinet
cd build-ouinet
cmake ${ANDROID_FLAGS} \
    -DANDROID=1 \
    -DWITH_INJECTOR=OFF \
    -DIFADDRS_SOURCES="${DIR}/android-ifaddrs/ifaddrs.c" \
    -DOPENSSL_INCLUDE_DIR=${SSL_DIR}/include \
    -DCMAKE_CXX_FLAGS="-I ${DIR}/android-ifaddrs -I $SSL_DIR/include" \
    ${ROOT}
make VERBOSE=1
cd -

add_library $DIR/build-ouinet/libclient.so
add_library $DIR/build-ouinet/modules/asio-ipfs/ipfs_bindings/libipfs_bindings.so
add_library $DIR/build-ouinet/gcrypt/src/gcrypt/src/.libs/libgcrypt.so
add_library $DIR/build-ouinet/gpg_error/out/lib/libgpg-error.so
}

######################################################################
function copy_jni_libs {
local jni_dst_dir="${DIR}"/build-android/builddir/deps/${ABI}
rm -rf "${jni_dst_dir}"
mkdir -p "${jni_dst_dir}"
local lib
for lib in "${OUT_LIBS[@]}"; do
    echo "Copying $lib to $jni_dst_dir"
    cp $lib $jni_dst_dir/
done
}

######################################################################
# Unpolished code to build the debug APK
function build_ouinet_apk {
mkdir -p "${DIR}"/build-android
cd "${DIR}"/build-android
ln -sf $(dirname ${APP_ROOT})/* .
export GRADLE_USER_HOME=$(pwd)/.gradle-home
gradle --no-daemon build \
    -Pboost_includedir=${BOOST_INCLUDEDIR} \
    -Pandroid_abi=${ABI} \
    -Pouinet_clientlib_path="${DIR}"/build-android/builddir/deps/${ABI}/libclient.so \
    -Plibdir="${DIR}"/build-android/builddir/deps

echo "---------------------------------"
echo "Your Android package is ready at:"
ls -l "$APK"
echo "---------------------------------"
cd -
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

setup_deps

if check_mode build; then
    maybe_install_ndk
    maybe_install_ndk_toolchain
    maybe_install_gradle
    maybe_install_boost
    maybe_install_openssl
    maybe_clone_ifaddrs
    # TODO: miniupnp
    build_ouinet_libs
    copy_jni_libs
    build_ouinet_apk
fi

if check_mode emu; then
    maybe_create_avd
    run_emulator "$@"
fi

# This only cleans files which may interfere when building for a different ABI,
# while keeping (some) downloaded and ABI-neutral stuff.
if check_mode abiclean; then
    rm -rf \
       Boost-for-Android/build \
       openssl-1.1.0g \
       build-ouinet \
       build-android/builddir
fi
