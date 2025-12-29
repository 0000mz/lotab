#!/bin/bash

MESON=${MESON:-meson}

BUILD_DIR="build"
BUILD_TYPE="debug"
DO_INSTALL=false

if [[ "$1" == "release" ]]; then
    BUILD_TYPE="release"
elif [[ "$1" == "install" ]]; then
    BUILD_TYPE="release"
    BUILD_DIR="build_release"
    DO_INSTALL=true
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "Setting up build directory $BUILD_DIR ($BUILD_TYPE)..."
    "$MESON" setup "$BUILD_DIR" --buildtype="$BUILD_TYPE"
else
    echo "Configuring build directory $BUILD_DIR to $BUILD_TYPE..."
    "$MESON" configure "$BUILD_DIR" -Dbuildtype="$BUILD_TYPE"
fi

echo "Compiling..."
"$MESON" compile -C "$BUILD_DIR"

if [ "$DO_INSTALL" = true ]; then
    echo "Installing..."
    "$MESON" install -C "$BUILD_DIR"
fi
