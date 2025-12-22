#!/bin/bash

BUILD_DIR="build"
BUILD_TYPE="debug"

if [[ "$1" == "release" ]]; then
    BUILD_TYPE="release"
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "Setting up build directory ($BUILD_TYPE)..."
    meson setup "$BUILD_DIR" --buildtype="$BUILD_TYPE"
else
    echo "Configuring build type to $BUILD_TYPE..."
    meson configure "$BUILD_DIR" -Dbuildtype="$BUILD_TYPE"
fi

echo "Compiling daemon..."
meson compile -C "$BUILD_DIR"
