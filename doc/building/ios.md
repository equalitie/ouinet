## iOS Framework

Ouinet can be built as an iOS framework to be included in iOS applications.

### Build requirements

A Mac computer running at least OSX 13.3. Ideally, with an Apple Silicon
processor and plenty of memory (>16GB). Note: these instructions have 
not been tested on older Intel-based Mac computers.

You will need git and Xcode 14 installed. We can't currently build with Xcode 15+ 
because it forces C++ 17, which is not yet supported by the ouinet source code.

If you have multiple versions of Xcode installed, you can switch between them with,
```
xcode-select --switch /Applications/Xcode14.app
```

### Building

First, clone my fork of the ouinet repo and checkout the ios-client branch.

```
git clone --recursive https://gitlab.com/paidforby/ouinet
cd ouinet
git checkout ios-client
```

It may be possible to build the iOS framework using the `build-ios.sh` script.
To do this, enter the scripts directory and execute the script,
```
cd scripts
./build-ios.sh
```

If this script is not successful, all the required steps can be performed manually
following the instructions shown below.  

Before building for iOS, you MUST build for macOS (technically, you only need to 
build the boost, gpg-error, and gcrypt libraries, but you might as well build the 
whole thing while you are at it). This is required to generate some of the helper 
binaries used when compiling for iOS.  

Create a directory for the macOS build inside of the ouinet repo and enter that directory,
```
mkdir build-macos
cd build-macos
```

Next, configure cmake for macOS target,
```
cmake .. -GXcode -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 -DCMAKE_BUILD_TYPE=Release
```

Then start the build
```
cmake --build . --config Release -- PRODUCT_BUNDLE_IDENTIFIER=org.equalitie.ouinet-macos DEVELOPMENT_TEAM=5SR9R72Z83
```

After the build finishes, exit the `build-macos` directory and create a directory 
for the iOS build and enter that directory
```
cd ..
mkdir build-ios
cd build-ios
```
Currently, we depend on leetal's [ios.toolchain.cmake](https://github.com/leetal/ios-cmake) 
file to correctly compile everything for iOS. The repo containing this is included 
as a submodule of the ouinet repo and should be checked out in the directory shown 
in the following command,
```
cmake .. -GXcode -DCMAKE_TOOLCHAIN_FILE=../ios/ouinet/toolchain/ios.toolchain.cmake -DPLATFORM=OS64 -DMACOS_BUILD_ROOT=$(realpath ../build-macos) -DCMAKE_BUILD_TYPE=Release
```
Note the MACOS_BUILT_ROOT argument, this is where you must point to the completed 
macOS build of ouinet.  

Next, run the build. It is preferable to start the build with `xcodebuild`, instead of cmake,
so we can remove `-parallelizeTargets` and add `-allowProvisioningUpdates` flags,
```
xcodebuild -project ouinet-iOS.xcodeproj build -target ALL_BUILD -configuration Release -hideShellScriptEnvironment ENABLE_BITCODE=0 PRODUCT_BUNDLE_IDENTIFIER=org.equalitie.ouinet-ios DEVELOPMENT_TEAM=5SR9R72Z83 -allowProvisioningUpdates
```

After the build completes, the resulting framework can be found in 
`build-ios/Release-iphoneos/ouinet-ios.framework`. To use the framework, copy the 
entire `ouinet-ios.framework` directory to your iOS Xcode project and import it via 
the Xcode GUI. Additionally, if changes have been made to the headers, you should 
copy them from `ios/ouinet/src/` directory in the ouinet source code to your iOS Xcode 
project, these include, `Ouinet.h`, `ouinet/OuinetClient.h`, `ouinet/OuinetConfig.h`. 
Also, the `Ouinet.h` file should be specified as your "Objective-C Bridging Header" in 
the configuration of your Xcode project.  

Once all of this is in place, you should be able to use the `OuinetClient` and 
`OuinetConfig` objects in your application code as shown in the [ouinet-example app](https://gitlab.com/equalitie/ouinetclient-ios).