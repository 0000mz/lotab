#!/bin/bash

MESON=${MESON:-meson}

BUILD_DIR="build"
BUILD_TYPE="debug"

if [[ "$1" == "release" ]]; then
    BUILD_TYPE="release"
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "Setting up build directory ($BUILD_TYPE)..."
    "$MESON" setup "$BUILD_DIR" --buildtype="$BUILD_TYPE"
else
    echo "Configuring build type to $BUILD_TYPE..."
    "$MESON" configure "$BUILD_DIR" -Dbuildtype="$BUILD_TYPE"
fi

echo "Compiling daemon..."
"$MESON" compile -C "$BUILD_DIR"
