#!/bin/bash

set -e

DIR=`pwd`
SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)

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

# https://developer.android.com/ndk/guides/abis.html
ABI=armeabi-v7a
#ABI=arm64-v8a

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

add_library $NDK_TOOLCHAIN_DIR/arm-linux-androideabi/lib/$CMAKE_SYSTEM_PROCESSOR/libc++_shared.so

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
        --with-libraries=regex,context,coroutine,program_options,system,test,thread,filesystem,date_time \
        $NDK_DIR
    cd ..
fi

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

######################################################################
ANDROID_FLAGS="\
    -DBoost_COMPILER='-clang' \
    -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_SYSTEM_VERSION=${NDK_PLATFORM} \
    -DCMAKE_ANDROID_STANDALONE_TOOLCHAIN=${NDK_TOOLCHAIN_DIR} \
    -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR} \
    -DCMAKE_ANDROID_ARCH_ABI=${ABI} \
    -DOPENSSL_ROOT_DIR=${SSL_DIR} \
    -DBOOST_INCLUDEDIR=${DIR}/Boost-for-Android/build/out/${ABI}/include/boost-${BOOST_V} \
    -DBOOST_LIBRARYDIR=${DIR}/Boost-for-Android/build/out/${ABI}/lib"

######################################################################
if [ ! -d "android-ifaddrs" ]; then
    # TODO: Still need to compile the .c file and make use of it.
    git clone https://github.com/PurpleI2P/android-ifaddrs.git
fi

######################################################################
# TODO: miniupnp
#   https://i2pd.readthedocs.io/en/latest/devs/building/android/

######################################################################
mkdir -p build-ouinet
cd build-ouinet
cmake ${ANDROID_FLAGS} \
    -DANDROID=1 \
    -DWITH_GNUNET=OFF \
    -DWITH_INJECTOR=OFF \
    -DIFADDRS_SOURCES="${DIR}/android-ifaddrs/ifaddrs.c" \
    -DOPENSSL_INCLUDE_DIR=${SSL_DIR}/include \
    -DCMAKE_CXX_FLAGS="-I ${DIR}/android-ifaddrs -I $SSL_DIR/include" \
    ${ROOT}
make VERBOSE=1
cd ..

add_library $DIR/build-ouinet/libclient.so
add_library $DIR/build-ouinet/modules/ipfs-cache/ipfs_bindings/libipfs_bindings.so
add_library $DIR/build-ouinet/modules/ipfs-cache/libipfs-cache.so

######################################################################
JNI_DST_DIR=${ROOT}/android/browser/src/main/jniLibs/${ABI}
rm -rf ${ROOT}/android/browser/src/main/jniLibs
mkdir -p $JNI_DST_DIR
for lib in "${OUT_LIBS[@]}"; do
    echo "Copying $lib to $JNI_DST_DIR"
    cp $lib $JNI_DST_DIR/
done

######################################################################
# Unpolished code to build the browser-debug.apk
#adb uninstall ie.equalit.ouinet
rm -rf android || true
rsync -r ../android .
cd android
GRADLE_USER_HOME=$DIR/.gradle-home
gradle --info -s --no-daemon build
adb devices
adb install -r ../android/browser/build/outputs/apk/debug/browser-debug.apk
adb logcat -c
adb logcat

######################################################################
