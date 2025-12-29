# Lotab Build Documentation

This project is a multi-component system designed to manage browser tabs via a macOS native application and a background daemon.

## Components

### 1. Chrome Extension (`extension/`)
- **Role**: Monitors tab and tab group lifecycle events (open, close, update) and forwards them to the daemon.
- **Technology**: Manifest V3, JavaScript.
- **Communication**: WebSocket client (`ws://localhost:9001`).

### 2. C Daemon (`daemon/`)
- **Role**: Central hub for data processing. It receives events from the extension, listens for global hotkeys, and spawns the GUI.
- **Technology**: C11, `libwebsockets`, `Carbon.framework`, Unix Domain Sockets.
- **Communication**:
    - WebSocket Server (port 9001).
    - UDS Client (`/tmp/lotab.sock`).

### 3. SwiftUI App (`app/`)
- **Role**: The graphical user interface for interacting with tab data.
- **Technology**: Swift, SwiftUI.
- **Communication**: UDS Server (`/tmp/lotab.sock`).

---

## Build Instructions

The project uses a unified **Meson** build system to compile both the C daemon and the Swift application.

### Prerequisites
- [Meson](https://mesonbuild.com/) v1.10.0 or later.
- [Ninja](https://ninja-build.org/) build tool (>=v1.13.2).
- [libwebsockets](https://libwebsockets.org/) (will be handled via Meson subproject if not found).
- macOS with Xcode Command Line Tools installed.

### Helper Script: `build.sh`
For convenience, a `build.sh` script is provided to automate the setup and compilation steps.

```bash
# Debug build (default)
./build.sh

# Release build
./build.sh release

# Build release and install
./build.sh install

# Install with custom prefix
PREFIX=/tmp/lotab_build ./build.sh install
```
The script automatically initializes the `build` directory if it doesn't exist and runs the compilation for all targets.

---

## Installation and Service Setup

You can install the application and configure it as a macOS system service ("LaunchAgent"). This ensures the daemon runs automatically in the background.

```bash
# 1. Build and Install (installs to /usr/local/ by default)
./build.sh install
```

# 2. Enable the Service
# Copy the installed plist to your LaunchAgents directory
cp /usr/local/share/lotab/com.mob.lotab.plist ~/Library/LaunchAgents/

# Load the service (using modern launchctl syntax)
# "gui/$(id -u)" targets your current user session
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.mob.lotab.plist
```

**Note**: If you used a custom prefix (e.g. `meson setup build --prefix /opt/local`), the plist will be in `<prefix>/share/lotab/`.

### Managing the Service
A helper script `scripts/launchctl.sh` is provided to simplify managing the LaunchAgent.

#### Load/Update Service
This copies the plist to `~/Library/LaunchAgents`, unloads any existing instance, and bootstraps the new one.
```bash
# If installed to default /usr/local
./scripts/launchctl.sh

# If installed to custom prefix
PREFIX=/opt/lotab_install ./scripts/launchctl.sh
```

#### Unload Service
Removes the service and deletes the plist from `~/Library/LaunchAgents`.
```bash
./scripts/launchctl.sh unload
```

### Troubleshooting `launchctl` Errors
If you start seeing `Bootstrap failed: 5: Input/output error`, it is usually due to incorrect file permissions on the plist file.

1.  **Check Ownership**: The plist in `~/Library/LaunchAgents/` MUST be owned by you, not `root`.
    ```bash
    ls -l ~/Library/LaunchAgents/com.mob.lotab.plist
    ```
2.  **Fix Permissions**:
    ```bash
    sudo chown $(id -un) ~/Library/LaunchAgents/com.mob.lotab.plist
    ```
3.  **Reset Service**:
    ```bash
    # Try to bootout first (ignore error if not found)
    launchctl bootout gui/$(id -u)/com.mob.lotab 2>/dev/null
    # Bootstrap again
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.mob.lotab.plist
    ```
4. **Check Status**: ` launchctl list | grep "com.mob.lotab"`
5. **See launchd logs associated with the application**: `log show --predicate 'sender == "launchd"' --last 3m | grep "com.mob.lotab"`

---
## Logs

When running as a system service, logs are stored in:
- **stdout**: `~/Library/Logs/lotab/stdout.log`
- **stderr**: `~/Library/Logs/lotab/stderr.log`

---

## Development & Debugging

### Running Locally (Without Install)
The daemon automagically finds the built `Lotab` app when running from the build directory.

```bash
./build/lotab_daemon
```

You can also explicitly point the daemon to a specific app binary:
```bash
./build/lotab_daemon --app-path ./build/Lotab
```

### Debugging with Sanitizers
The build system provides specialized variants for memory and thread safety analysis:

- **AddressSanitizer (ASan)**: Detects memory corruption/leaks.
  - Binaries: `build/lotab_daemon_asan`, `build/Lotab_asan`
- **ThreadSanitizer (TSan)**: Detects data races.
  - Binaries: `build/lotab_daemon_tsan`, `build/Lotab_tsan`

---

## Testing

The project includes automated functional tests integrated into the Meson build system.

### Prerequisites for Testing
- [uv](https://github.com/astral-sh/uv): Used to manage Python test dependencies (`psutil`).
  - Install via: `curl -LsSf https://astral.sh/uv/install.sh | sh` (or brew, pip, etc.)

### Running Tests
To run the full test suite via Meson:

```bash
meson test -C build
```

or for verbose output:

```bash
meson test -C build -v
```

### UI Tests (macOS)
The suite includes interaction tests (`tests/ui_test_quit.py`) that verify the daemon and GUI application behavior using AppleScript.
> [!IMPORTANT]
> Use of accessibility features (via `osascript`) requires granting "Accessibility" permissions to your terminal or editor in System Settings -> Privacy & Security.

#### Manual Execution
You can also run the test script manually using `uv`. You must provide the path to the daemon executable:

```bash
uv run tests/ui_test_quit.py ./build/lotab_daemon
```
