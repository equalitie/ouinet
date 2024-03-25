#!/bin/bash

set -e
set -x

SCRIPT_DIR=$(dirname -- "$(readlink -f -- "$BASH_SOURCE")")
ROOT=$(cd ${SCRIPT_DIR}/.. && pwd)

IOS_TOOLCHAIN_PATH=../ios/ouinet/toolchain/ios.toolchain.cmake

MACOS_BUILD_ROOT=$ROOT/build-macos
IOS_BUILD_ROOT=$ROOT/build-ios

mkdir $MACOS_BUILD_ROOT

pushd $MACOS_BUILD_ROOT
cmake .. -GXcode -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -- PRODUCT_BUNDLE_IDENTIFIER=ie.equalit.ouinet-macos DEVELOPMENT_TEAM=5SR9R72Z83
popd

mkdir $IOS_BUILD_ROOT

pushd $IOS_BUILD_ROOT
cmake .. -GXcode -DCMAKE_TOOLCHAIN_FILE=$IOS_TOOLCHAIN_PATH -DPLATFORM=OS64 -DMACOS_BUILD_ROOT=$(realpath ../build-macos) -DCMAKE_BUILD_TYPE=Release
xcodebuild -project ouinet-iOS.xcodeproj build -target ALL_BUILD -configuration Release -hideShellScriptEnvironment ENABLE_BITCODE=0 PRODUCT_BUNDLE_IDENTIFIER=org.equalitie.ouinet-ios DEVELOPMENT_TEAM=5SR9R72Z83 -allowProvisioningUpdates
popd