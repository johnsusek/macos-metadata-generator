# macos-metadata-generator

Generates Swift JSExport classes and TS types for MacOS

## Usage

```bash
brew install yaml-cpp
mkdir build && cd build
cmake -G Xcode -DCMAKE_CXX_COMPILER="/usr/bin/g++" -DCMAKE_C_COMPILER="/usr/bin/gcc" -DCMAKE_PREFIX_PATH="/usr/local/cellar/yaml-cpp/0.6.3_1/" ..
```

## Run

```bash
export SDKROOT="/Library/Developer/CommandLineTools/SDKs/MacOSX10.15.sdk/"
export SDKVERSION="10.15"
export SWIFTVERSION="5.3"
export ATTRIBUTESPATH="../../data/attributes/"

# xcodebuild
# -or-
# if you want to interactively debug in xcode
# open the xcode project
# Project settings:
# Base SDK: "/Library/Developer/CommandLineTools/SDKs/MacOSX10.15.sdk"
# Edit Scheme: add the objc-metadata-generator arguments from below & and the ENV variables from above
# build and run xcode project

cd build/bin
./objc-metadata-generator -output-typescript ../../../data/types -output-jsexport ../../../data/jsexport -blacklist-modules-file ../../blacklist -whitelist-modules-file ../../whitelist Xclang -Wno-everything
```

# Credits

Based on [ios-metadata-generator](https://github.com/NativeScript/ios-metadata-generator).
