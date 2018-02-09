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
if [ "$ABI" = "armeabi-v7a" ]; then
    CMAKE_SYSTEM_PROCESSOR="armv7-a"
else
    >&2 echo "TODO: Need a mapping from \"$ABI\" to CMAKE_SYSTEM_PROCESSOR"
    exit 1
fi

######################################################################
if [ ! -d "./$NDK" ]; then
    cd /tmp
    wget https://dl.google.com/android/repository/${NDK_ZIP}
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

######################################################################
if [ ! -d "Boost-for-Android" ]; then
    git clone https://github.com/inetic/Boost-for-Android
fi

if [ ! -d "Boost-for-Android/build" ]; then
    cd Boost-for-Android
    ./build-android.sh \
        --boost=${BOOST_V_DOT} \
        --arch=${ABI} \
        --with-libraries=context,coroutine,program_options,system,test,thread,filesystem,date_time \
        $NDK_DIR
    cd ..
fi

######################################################################
mkdir -p build-ipfs-cache
cd build-ipfs-cache

cmake \
    -DBoost_COMPILER="-clang" \
    -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_SYSTEM_VERSION=${NDK_PLATFORM} \
    -DCMAKE_ANDROID_STANDALONE_TOOLCHAIN=${NDK_TOOLCHAIN_DIR} \
    -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR} \
    -DCMAKE_ANDROID_ARCH_ABI=${ABI} \
    -DBOOST_INCLUDEDIR=${DIR}/Boost-for-Android/build/out/${ABI}/include/boost-${BOOST_V} \
    -DBOOST_LIBRARYDIR=${DIR}/Boost-for-Android/build/out/${ABI}/lib \
    ${ROOT}/modules/ipfs-cache

make

cd ..
