#!/bin/bash

set -e

DIR=`pwd`
SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)

NDK=android-ndk-r16b
NDK_DIR=$DIR/$NDK
NDK_ZIP=${NDK}-linux-x86_64.zip

NDK_PLATFORM=19
NDK_ARCH=arm
NDK_STL='libc++'
NDK_TOOLCHAIN_DIR=${DIR}/${NDK}-toolchain-android$NDK_PLATFORM-$NDK_ARCH-$NDK_STL

BOOST_V=1_65_1
BOOST_V_DOT=${BOOST_V//_/.} # 1.65.1

# https://developer.android.com/ndk/guides/abis.html
ABI=armeabi-v7a

######################################################################
# This variable shall contain paths to generated libraries which
# must all be included in the final Android package.
OUT_LIBS=()

function add_library {
    local libs=("$@")
    for lib in "${libs[@]}"; do
        if [ ! -f "$lib" ]; then
            echo "Cannot add library \"$lib\": File doesn't exist"
            exit 1
        fi
        OUT_LIBS+=("$lib")
    done
}

######################################################################
which unzip > /dev/null || sudo apt-get install unzip

######################################################################
if [ "$ABI" = "armeabi-v7a" ]; then
    CMAKE_SYSTEM_PROCESSOR="armv7-a"
else
    >&2 echo "TODO: Need a mapping from \"$ABI\" to CMAKE_SYSTEM_PROCESSOR"
    exit 1
fi

######################################################################
if [ ! -d "./$NDK" ]; then
    cd /tmp
    if [ ! -f ${NDK_ZIP} ]; then
        wget https://dl.google.com/android/repository/${NDK_ZIP}
    fi
    cd ${DIR}
    unzip /tmp/${NDK_ZIP}
    rm /tmp/${NDK_ZIP}
fi

######################################################################
if [ ! -d "${NDK_TOOLCHAIN_DIR}" ]; then
    $NDK_DIR/build/tools/make-standalone-toolchain.sh \
        --platform=android-$NDK_PLATFORM \
        --arch=$NDK_ARCH \
        --stl=$NDK_STL \
        --install-dir=${NDK_TOOLCHAIN_DIR}
fi

export ANDROID_NDK_HOME=$DIR/android-ndk-r16b

######################################################################
if [ ! -d "./gradle-4.6" ]; then
    wget https://services.gradle.org/distributions/gradle-4.6-bin.zip
    # TODO: Check SHA256
    unzip gradle-4.6-bin.zip
    rm gradle-4.6-bin.zip
fi

export PATH="`pwd`/gradle-4.6/bin:$PATH"

######################################################################
if [ ! -d "Boost-for-Android" ]; then
    git clone https://github.com/inetic/Boost-for-Android
fi

if [ ! -d "Boost-for-Android/build" ]; then
    cd Boost-for-Android
    # TODO: Android doesn't need program_options and test.
    ./build-android.sh \
        --boost=${BOOST_V_DOT} \
        --arch=${ABI} \
        --with-libraries=context,coroutine,program_options,system,test,thread,filesystem,date_time \
        $NDK_DIR
    cd ..
fi

add_library $DIR/Boost-for-Android/build/out/armeabi-v7a/lib/*

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

SSL_V="1.1.0g"
SSL_DIR=${DIR}/openssl-${SSL_V}

if [ ! -d "$SSL_DIR" ]; then
    if [ ! -f openssl-${SSL_V}.tar.gz ]; then
        wget https://www.openssl.org/source/openssl-${SSL_V}.tar.gz
    fi
    tar xf openssl-${SSL_V}.tar.gz
    (cd $SSL_DIR && build_openssl)
fi

add_library $DIR/openssl-1.1.0g/libcrypto.a
add_library $DIR/openssl-1.1.0g/libssl.a

######################################################################
ANDROID_FLAGS="\
    -DBoost_COMPILER='-clang' \
    -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_SYSTEM_VERSION=${NDK_PLATFORM} \
    -DCMAKE_ANDROID_STANDALONE_TOOLCHAIN=${NDK_TOOLCHAIN_DIR} \
    -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR} \
    -DCMAKE_ANDROID_ARCH_ABI=${ABI} \
    -DBOOST_INCLUDEDIR=${DIR}/Boost-for-Android/build/out/${ABI}/include/boost-${BOOST_V} \
    -DBOOST_LIBRARYDIR=${DIR}/Boost-for-Android/build/out/${ABI}/lib"

######################################################################
if [ ! -d "build-ipfs-cache" ]; then
    mkdir -p build-ipfs-cache
    cd build-ipfs-cache
    cmake ${ANDROID_FLAGS} ${ROOT}/modules/ipfs-cache
    make
    cd ..
fi

add_library $DIR/build-ipfs-cache/ipfs_bindings/ipfs_bindings.so
add_library $DIR/build-ipfs-cache/libipfs-cache.so

######################################################################
if [ ! -d "android-ifaddrs" ]; then
    # TODO: Still need to compile the .c file and make use of it.
    git clone https://github.com/PurpleI2P/android-ifaddrs.git
fi

# TODO: Compile ifaddrs.c and add it to libraries
# add_library ./.../ifaddrs.a

######################################################################
# TODO: Missing dependencies for i2pd:
#   * git clone https://github.com/PurpleI2P/MiniUPnP-for-Android-Prebuilt.git
# As described here:
#   https://i2pd.readthedocs.io/en/latest/devs/building/android/

#rm -rf build-i2poui
mkdir -p build-i2poui
cd build-i2poui

cmake \
    ${ANDROID_FLAGS} \
    -DANDROID=1 \
    -DOPENSSL_INCLUDE_DIR=${SSL_DIR}/include \
    -DCMAKE_CXX_FLAGS="-I ${DIR}/android-ifaddrs -I $SSL_DIR/include" \
    ${ROOT}/modules/i2pouiservice

make VERBOSE=1
cd ..

add_library $DIR/build-i2poui/i2pd/build/libi2pd.a
add_library $DIR/build-i2poui/i2pd/build/libi2pdclient.a
add_library $DIR/build-i2poui/libi2poui.a

######################################################################
#adb uninstall ie.equalit.ouinet
rm -rf android || true
rsync -r ../android .
cd android
GRADLE_USER_HOME=$DIR/.gradle-home
gradle -s --no-daemon build
adb devices
adb install ../android/browser/build/outputs/apk/debug/browser-debug.apk

######################################################################
