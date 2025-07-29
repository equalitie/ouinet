#!/bin/bash

set -e
set -x

DIR=`pwd`
SCRIPT_DIR=$(dirname -- "$(readlink -f -- "${BASH_SOURCE[@]}")")
ROOT=$(cd "${SCRIPT_DIR}"/.. && pwd)

BUILD_TYPE='Debug'
while getopts r option; do
    case "$option" in
        r) BUILD_TYPE='Release';;
        *) echo "Default to debug build";;
    esac
done

create_framework() {
    local DYLIB_PATH=$1
    local HEADERS_PATH=$2
    local OUTPUT_PATH=$3
    local PLATFORM=$4  # "iphoneos" or "iphonesimulator"
    local MIN_OS_VERSION=${5:-"12.0"}  # Default to iOS 12.0
    local FRAMEWORK_NAME="ouinet"

    # Determine platform string for plist
    if [[ "$PLATFORM" == *"simulator"* ]]; then
        PLATFORM_STRING="iPhoneSimulator"
    else
        PLATFORM_STRING="iPhoneOS"
    fi
    
    # Create framework structure
    mkdir -p "${OUTPUT_PATH}/Headers"
    mkdir -p "${OUTPUT_PATH}/Modules"
    
    # Copy dylib as framework binary
    cp "${DYLIB_PATH}" "${OUTPUT_PATH}/${FRAMEWORK_NAME}"
    
    # Copy headers
    cp -R "${HEADERS_PATH}/"* "${OUTPUT_PATH}/Headers/"
    
    # Create Info.plist
    cat > "${OUTPUT_PATH}/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>${FRAMEWORK_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>ie.equalit.${FRAMEWORK_NAME}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${FRAMEWORK_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>FMWK</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>MinimumOSVersion</key>
    <string>${MIN_OS_VERSION}</string>
    <key>CFBundleSupportedPlatforms</key>
    <array>
        <string>${PLATFORM_STRING}</string>
    </array>
</dict>
</plist>
EOF
    
    echo "Framework created at: ${OUTPUT_PATH}"

    # Create module.modulemap
cat > "${OUTPUT_PATH}/Modules/module.modulemap" << EOF
framework module ${FRAMEWORK_NAME} {
  umbrella header "${FRAMEWORK_NAME}.h"
  export *
  module * { export * }
}
EOF
}

# Create frameworks for both architectures
create_framework \
    "${DIR}/build-os64/${BUILD_TYPE}-iphoneos/ouinet" \
    "${ROOT}/ios/ouinet/include" \
    "${DIR}/build-os64/${BUILD_TYPE}-iphoneos/ouinet.framework" \
    "iphoneos" \
    "15.0"

#create_framework \
#    "${DIR}/build-simulatorarm64/${BUILD_TYPE}-iphonesimulator/ouinet" \
#    "${ROOT}/ios/ouinet/include" \
#    "${DIR}/build-simulatorarm64/${BUILD_TYPE}-iphonesimulator/ouinet.framework" \
#    "iphonesimulator" \
#    "15.0"