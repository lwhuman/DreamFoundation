#!/usr/bin/env sh

git submodule update --init --recursive

mkdir build

python setup.py

# BUILD_TYPE=Debug
BUILD_TYPE=Release

cd build
cmake .. -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build . --config "$BUILD_TYPE"
