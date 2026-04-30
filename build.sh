#!/usr/bin/env sh

# BUILD_TYPE=Debug
BUILD_TYPE=Release

cd build
cmake .. -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build . --config "$BUILD_TYPE"
