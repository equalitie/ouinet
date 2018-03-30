#!/bin/bash

set -e

DIR=`pwd`
SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)
APP_ROOT="${ROOT}/android/browser"
APK="${APP_ROOT}/build/outputs/apk/debug/browser-debug.apk"
APK_ID=$(sed -En 's/^\s*\bapplicationId\s+"([^"]+)".*/\1/p' "${APP_ROOT}/build.gradle")

NDK=android-ndk-r16b
NDK_DIR=$DIR/$NDK
NDK_ZIP=${NDK}-linux-x86_64.zip

# `posix_fadvise`, required by Boost.Beast is was only added in LOLLIPOP
# https://developer.android.com/guide/topics/manifest/uses-sdk-element.html#ApiLevels
NDK_PLATFORM=21
NDK_ARCH=arm
NDK_STL='libc++'
NDK_TOOLCHAIN_DIR=${DIR}/${NDK}-toolchain-android$NDK_PLATFORM-$NDK_ARCH-$NDK_STL

BOOST_V=1_65_1
BOOST_V_DOT=${BOOST_V//_/.} # 1.65.1

EMULATOR_AVD=ouinet-test

# The following options may be limited by availability of SDK packages,
# to get list of all packages, use `sdkmanager --list`.
# https://developer.android.com/ndk/guides/abis.html
ABI=armeabi-v7a
#ABI=arm64-v8a

# Android API level
PLATFORM=android-25

# The image to be used by the emulator AVD
EMULATOR_IMAGE_TAG=google_apis
EMULATOR_IMAGE="system-images;$PLATFORM;$EMULATOR_IMAGE_TAG;$ABI"

# To get list of all devices, use `avdmanager list device`.
EMULATOR_DEV="Nexus 6"
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
ALLOWED_MODES="build emu"
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
SDK="$DIR/sdk"
local toolsfile=sdk-tools-linux-3859397.zip
local sdkmanager="$SDK/tools/bin/sdkmanager"

if [ ! -f "$sdkmanager" ]; then
    [ -d "$SDK/tools" ] || rm -rf "$SDK/tools"
    if [ ! -f "$toolsfile" ]; then
        # https://developer.android.com/studio/index.html#command-tools
        wget "https://dl.google.com/android/repository/$toolsfile"
    fi
    unzip "$toolsfile" -d "$SDK"
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
            if [ ! -d "$SDK/$(echo $pkg | tr ';' /)" ]; then
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
export PATH="$SDK/platform-tools:$PATH"

export ANDROID_HOME=$(dirname $(dirname $(which adb)))
}

######################################################################
# Create Android virtual device for the emulator.
function maybe_create_avd {
if ! "$SDK/tools/emulator" -list-avds | grep -q "^$EMULATOR_AVD$"; then
    echo no | "$SDK/tools/bin/avdmanager" create avd -n "$EMULATOR_AVD" \
                                          -k "$EMULATOR_IMAGE" -g "$EMULATOR_IMAGE_TAG" \
                                          -b "$ABI" -d "$EMULATOR_DEV"
fi
}

######################################################################
if [ "$ABI" = "armeabi-v7a" ]; then
    CMAKE_SYSTEM_PROCESSOR="armv7-a"
elif [ "$ABI" = "arm64-v8a" ]; then
    CMAKE_SYSTEM_PROCESSOR="aarch64"
elif [ "$ABI" = "armeabi" ]; then
    CMAKE_SYSTEM_PROCESSOR="armv5te"
elif [ "$ABI" = "x86" ]; then
    CMAKE_SYSTEM_PROCESSOR="i686"
elif [ "$ABI" = "x86_64" ]; then
    CMAKE_SYSTEM_PROCESSOR="x86_64"
else
    # This may help:
    # https://github.com/opencv/opencv/blob/5b868ccd829975da5372bf330994553e176aee09/platforms/android/android.toolchain.cmake#L658
    >&2 echo "TODO: Need a mapping from \"$ABI\" to CMAKE_SYSTEM_PROCESSOR"
    exit 1
fi

######################################################################
function maybe_install_ndk {
if [ ! -d "./$NDK" ]; then
    cd /tmp
    if [ ! -f ${NDK_ZIP} ]; then
        wget https://dl.google.com/android/repository/${NDK_ZIP}
    fi
    cd ${DIR}
    unzip /tmp/${NDK_ZIP}
    rm /tmp/${NDK_ZIP}
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

export ANDROID_NDK_HOME=$DIR/android-ndk-r16b

add_library $NDK_TOOLCHAIN_DIR/arm-linux-androideabi/lib/$CMAKE_SYSTEM_PROCESSOR/libc++_shared.so
}

######################################################################
function maybe_install_gradle {
if [ ! -d "./gradle-4.6" ]; then
    wget https://services.gradle.org/distributions/gradle-4.6-bin.zip
    # TODO: Check SHA256
    unzip gradle-4.6-bin.zip
    rm gradle-4.6-bin.zip
fi

export PATH="`pwd`/gradle-4.6/bin:$PATH"
}

######################################################################
function maybe_install_boost {
if [ ! -d "Boost-for-Android" ]; then
    git clone https://github.com/inetic/Boost-for-Android
fi

if [ ! -d "Boost-for-Android/build" ]; then
    cd Boost-for-Android
    # TODO: Android doesn't need program_options and test.
    ./build-android.sh \
        --boost=${BOOST_V_DOT} \
        --arch=${ABI} \
        --with-libraries=regex,context,coroutine,program_options,system,test,thread,filesystem,date_time \
        $NDK_DIR
    cd ..
fi
}

######################################################################
function build_openssl {
    export SYSTEM=android
    export CROSS_SYSROOT="$NDK_TOOLCHAIN_DIR/sysroot"
    export ANDROID_DEV="$SYSROOT/usr"
    export MACHINE=armv7
    export CROSS_COMPILE=arm-linux-androideabi-
    export TOOLCHAIN="$NDK_TOOLCHAIN_DIR"
    export PATH="$NDK_TOOLCHAIN_DIR/bin:$PATH"
    ./config -v no-shared -no-ssl2 -no-ssl3 -no-comp -no-hw -no-engine
    make depend
    make build_libs
}

function maybe_install_openssl {
local ssl_v="1.1.0g"
SSL_DIR=${DIR}/openssl-${ssl_v}

if [ ! -d "$SSL_DIR" ]; then
    if [ ! -f openssl-${ssl_v}.tar.gz ]; then
        wget https://www.openssl.org/source/openssl-${ssl_v}.tar.gz
    fi
    tar xf openssl-${ssl_v}.tar.gz
    (cd $SSL_DIR && build_openssl)
fi
}

######################################################################
BOOST_INCLUDEDIR=${DIR}/Boost-for-Android/build/out/${ABI}/include/boost-${BOOST_V}
BOOST_LIBRARYDIR=${DIR}/Boost-for-Android/build/out/${ABI}/lib

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
cd ..

add_library $DIR/build-ouinet/libclient.so
add_library $DIR/build-ouinet/modules/ipfs-cache/ipfs_bindings/libipfs_bindings.so
}

######################################################################
function copy_jni_libs {
local jni_dst_dir=${APP_ROOT}/src/main/jniLibs/${ABI}
rm -rf ${APP_ROOT}/src/main/jniLibs
mkdir -p $jni_dst_dir
local lib
for lib in "${OUT_LIBS[@]}"; do
    echo "Copying $lib to $jni_dst_dir"
    cp $lib $jni_dst_dir/
done
}

######################################################################
# Unpolished code to build the debug APK
function build_ouinet_apk {
cd $(dirname ${APP_ROOT})
export GRADLE_USER_HOME=$DIR/.gradle-home
gradle --no-daemon build -Pboost_includedir=${BOOST_INCLUDEDIR}

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
    "$SDK/tools/emulator" -avd "$EMULATOR_AVD" -skin "$EMULATOR_SKIN" \
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
