#!/bin/bash

set -e
MESON=${MESON:-meson}

REQUIRED_VERSION="1.10.0"
CURRENT_VERSION=$("$MESON" --version)

if [ "$CURRENT_VERSION" != "$REQUIRED_VERSION" ]; then
    echo "Error: Meson version $REQUIRED_VERSION is required, but found $CURRENT_VERSION"
    exit 1
fi

BUILD_DIR="build/debug"
BUILD_TYPE="debug"
DO_INSTALL=false

if [[ "$1" == "release" ]]; then
    BUILD_TYPE="release"
    BUILD_DIR="build/release"
elif [[ "$1" == "install" ]]; then
    BUILD_TYPE="release"
    BUILD_DIR="build/release"
    DO_INSTALL=true
fi

EXTRA_SETUP_ARGS=""
EXTRA_CONF_ARGS=""

if [ -n "$PREFIX" ]; then
    echo "Using install prefix: $PREFIX"
    EXTRA_SETUP_ARGS="--prefix=$PREFIX"
    EXTRA_CONF_ARGS="-Dprefix=$PREFIX"
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "Setting up build directory $BUILD_DIR ($BUILD_TYPE)..."
    "$MESON" setup "$BUILD_DIR" --buildtype="$BUILD_TYPE" $EXTRA_SETUP_ARGS
else
    echo "Configuring build directory $BUILD_DIR to $BUILD_TYPE..."
    "$MESON" configure "$BUILD_DIR" -Dbuildtype="$BUILD_TYPE" $EXTRA_CONF_ARGS
fi

echo "Compiling..."
"$MESON" compile -C "$BUILD_DIR"

if [ "$DO_INSTALL" = true ]; then
    echo "Installing..."
    "$MESON" install -C "$BUILD_DIR"
fi
