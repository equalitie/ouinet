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
    ./build-android.sh --arch=${ABI} --with-libraries=context,coroutine,program_options,system,test,thread,filesystem,date_time $NDK_DIR
    cd ..
fi

######################################################################
TOOLCHAIN_FILE=${DIR}/toolchain-android${NDK_PLATFORM}-${NDK_ARCH}-llvm.cmake
if [ -f "$TOOLCHAIN_FILE" ]; then rm "$TOOLCHAIN_FILE"; fi

echo "set(CMAKE_SYSTEM_NAME Android)"                                          >> ${TOOLCHAIN_FILE}
echo "set(CMAKE_SYSTEM_VERSION ${NDK_PLATFORM})"                               >> ${TOOLCHAIN_FILE}
echo "set(CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR})"                   >> ${TOOLCHAIN_FILE}
echo "set(CMAKE_ANDROID_ARCH_ABI ${ABI})"                                      >> ${TOOLCHAIN_FILE}
echo "set(CMAKE_ANDROID_STANDALONE_TOOLCHAIN ${NDK_TOOLCHAIN_DIR})"            >> ${TOOLCHAIN_FILE}
echo "set(BOOST_INCLUDEDIR ${DIR}/Boost-for-Android/build/out/${ABI}/include/boost-1_65_1)" >> ${TOOLCHAIN_FILE}
echo "set(BOOST_LIBRARYDIR ${DIR}/Boost-for-Android/build/out/${ABI}/lib)"     >> ${TOOLCHAIN_FILE}
echo "set(CMAKE_C_COMPILER ${NDK_TOOLCHAIN_DIR}/bin/clang)"                    >> ${TOOLCHAIN_FILE}
echo "set(CMAKE_CXX_COMPILER ${NDK_TOOLCHAIN_DIR}/bin/clang++)"                >> ${TOOLCHAIN_FILE}

######################################################################
mkdir -p build-ipfs-cache
cd build-ipfs-cache

cmake -DBoost_COMPILER="-clang" \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE \
    ${ROOT}/modules/ipfs-cache
make -B

cd ..
