#!/bin/bash

set -e
set -x

SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)

BUILD_TYPE=${1:-'Release'}

IOS_TOOLCHAIN_PATH=../ios/ouinet/toolchain/ios.toolchain.cmake

MACOS_BUILD_ROOT=$ROOT/build-macos
IOS_BUILD_ROOT=$ROOT/build-ios

mkdir $MACOS_BUILD_ROOT

pushd $MACOS_BUILD_ROOT
cmake .. -GXcode -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_TOOLCHAIN_FILE=$(realpath $IOS_TOOLCHAIN_PATH) -DPLATFORM=MAC_ARM64
cmake --build . --config ${BUILD_TYPE} -- PRODUCT_BUNDLE_IDENTIFIER=ie.equalit.ouinet-macos DEVELOPMENT_TEAM=5SR9R72Z83
popd

mkdir $IOS_BUILD_ROOT

pushd $IOS_BUILD_ROOT
cmake .. -GXcode -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_TOOLCHAIN_FILE=$(realpath $IOS_TOOLCHAIN_PATH) -DPLATFORM=OS64 -DMACOS_BUILD_ROOT=$(realpath ../build-macos)
xcodebuild -project ouinet-iOS.xcodeproj build -target ALL_BUILD -configuration ${BUILD_TYPE} -hideShellScriptEnvironment ENABLE_BITCODE=0 PRODUCT_BUNDLE_IDENTIFIER=org.equalitie.ouinet-ios DEVELOPMENT_TEAM=5SR9R72Z83 -allowProvisioningUpdates
popd