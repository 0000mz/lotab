# Tab Manager Build Documentation

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
    - UDS Client (`/tmp/tabmanager.sock`).

### 3. SwiftUI App (`app/`)
- **Role**: The graphical user interface for interacting with tab data.
- **Technology**: Swift, SwiftUI.
- **Communication**: UDS Server (`/tmp/tabmanager.sock`).

---

## Build Instructions

The project uses a unified **Meson** build system to compile both the C daemon and the Swift application.

### Prerequisites
- [Meson](https://mesonbuild.com/) 1.0 or later.
- [Ninja](https://ninja-build.org/) build tool.
- [libwebsockets](https://libwebsockets.org/) (will be handled via Meson subproject if not found).
- macOS with Xcode Command Line Tools installed.

### Helper Script: `build.sh`
For convenience, a `build.sh` script is provided to automate the setup and compilation steps.

```bash
# Debug build (default)
./build.sh

# Release build
./build.sh release
```
The script automatically initializes the `build` directory if it doesn't exist and runs the compilation for all targets.

### Debugging with Sanitizers
The build system provides specialized variants for memory and thread safety analysis:

- **AddressSanitizer (ASan)**: Detects memory corruption/leaks.
  - Binaries: `build/daemon_asan`, `build/TabManager_asan`
- **ThreadSanitizer (TSan)**: Detects data races.
  - Binaries: `build/daemon_tsan`, `build/TabManager_tsan`

---

## Running the Application

The daemon is designed to be the primary entry point. It automatically verifies its configuration and spawns the corresponding GUI application.

```bash
# Start the standard system
./build/daemon

# Start with AddressSanitizer
./build/daemon_asan

# Start with ThreadSanitizer
./build/daemon_tsan
```

The Chrome extension must be loaded manually into Chrome via "Load unpacked" from the `extension/` directory.

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
uv run tests/ui_test_quit.py ./build/daemon
```
