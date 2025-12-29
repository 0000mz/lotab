#!/bin/bash
set -e

# Usage:
#   PREFIX=/opt/homebrew ./scripts/launchctl.sh         # Load/Update service
#   ./scripts/launchctl.sh unload                       # Unload service

# Default prefix if not set
PREFIX="${PREFIX:-/usr/local}"
SERVICE_NAME="${LOTAB_SERVICE_NAME:-com.mob.lotab}"
PLIST_NAME="${SERVICE_NAME}.plist"
USER_ID=$(id -u)
SERVICE_TARGET="gui/${USER_ID}"
LAUNCH_AGENTS_DIR="$HOME/Library/LaunchAgents"
DEST_PLIST="${LAUNCH_AGENTS_DIR}/${PLIST_NAME}"

# Source plist location (installed location)
SOURCE_PLIST="${PREFIX}/share/lotab/${PLIST_NAME}"

function log() {
    echo "==> $1"
}

function unload() {
    log "Unloading service $SERVICE_NAME..."
    # Don't fail if not currently loaded or service specifier not found
    if launchctl print "$SERVICE_TARGET/$SERVICE_NAME" >/dev/null 2>&1; then
        launchctl bootout "$SERVICE_TARGET/$SERVICE_NAME" || echo "Warning: bootout failed"
    else
        log "Service not currently running."
    fi

    if [ -f "$DEST_PLIST" ]; then
        log "Removing plist from $DEST_PLIST"
        rm "$DEST_PLIST"
    fi
}

function load() {
    if [ ! -f "$SOURCE_PLIST" ]; then
        echo "Error: Plist not found at $SOURCE_PLIST"
        echo "Ensure PREFIX is set correctly (current: $PREFIX) and app is installed."
        exit 1
    fi

    # Unload first to kill previous instance and cleanup
    unload

    log "Copying plist to $DEST_PLIST"
    mkdir -p "$LAUNCH_AGENTS_DIR"
    mkdir -p "$HOME/Library/Logs/lotab"

    cp "$SOURCE_PLIST" "$DEST_PLIST"

    log "Bootstrapping service into $SERVICE_TARGET..."
    launchctl bootstrap "$SERVICE_TARGET" "$DEST_PLIST"

    log "Service status:"
    # Give it a moment to register?
    sleep 0.5
    launchctl list | grep "com.mob.lotab"
}

if [ "$1" == "unload" ]; then
    unload
else
    load
fi
