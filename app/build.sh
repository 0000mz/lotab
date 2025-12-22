#!/bin/bash

CONFIGURATION="Debug"

if [[ "$1" == "release" ]]; then
    CONFIGURATION="Release"
fi

echo "Building configuration: $CONFIGURATION"
xcodebuild -scheme TabManagerApp \
    -configuration "$CONFIGURATION" \
    -derivedDataPath ./build \
    -destination 'platform=macOS'
