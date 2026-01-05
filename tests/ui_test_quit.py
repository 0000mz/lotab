# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "psutil",
#     "pytest",
# ]
# ///

import os
import time
import subprocess
import psutil
import pytest

APP_NAME = "Lotab"


def get_name_of_bin(path: str) -> str:
    return os.path.basename(path)


def get_pids_by_name(name):
    pids = []
    for proc in psutil.process_iter(["pid", "name", "cmdline"]):
        try:
            # Match if name is in process name OR if it's in the command line (for daemon arg 0)
            if name in proc.info["name"] or (
                proc.info["cmdline"] and name in proc.info["cmdline"][0]
            ):
                pids.append(proc.info["pid"])
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass
    return pids


def kill_processes_by_name(name):
    for proc in psutil.process_iter(["pid", "name", "cmdline"]):
        try:
            if name in proc.info["name"] or (
                proc.info["cmdline"] and name in proc.info["cmdline"][0]
            ):
                proc.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass


@pytest.fixture(scope="function")
def daemon_bin():
    # Allow overriding via environment variable, default to debug build
    path = os.environ.get("DAEMON_BIN", "./build/debug/lotab_daemon_asan")
    if not os.path.exists(path):
        pytest.fail(f"Daemon binary not found at {path}. Build the project first.")
    return path


@pytest.fixture(scope="function")
def daemon_process(daemon_bin):
    """Fixture to start and manage the daemon process."""
    print(f"\n[Fixture] Setting up daemon from {daemon_bin}...")

    # 1. Cleanup previous instances
    bin_name = get_name_of_bin(daemon_bin)
    kill_processes_by_name(bin_name)
    kill_processes_by_name(APP_NAME)
    time.sleep(1)

    # 2. Start Daemon
    proc = subprocess.Popen(
        [daemon_bin], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Wait for startup
    time.sleep(5)

    if proc.poll() is not None:
        pytest.fail("Daemon failed to start or exited immediately.")

    print(f"[Fixture] Daemon started with PID: {proc.pid}")

    yield proc

    # Teardown
    print("\n[Fixture] Tearing down daemon...")
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    # Cleanup child GUI processes
    kill_processes_by_name(APP_NAME)


def test_quit_via_menu(daemon_process, daemon_bin):
    """Test that the application quits correctly via the menu item."""

    # 1. Verify Child App Spawned
    # Excluding the daemon process itself from the check
    app_pids = get_pids_by_name(APP_NAME)
    app_pids = [p for p in app_pids if p != daemon_process.pid]

    if not app_pids:
        pytest.fail(f"{APP_NAME} GUI did not spawn.")

    print(f"{APP_NAME} running with PID(s): {app_pids}")

    # 2. Interact with UI via AppleScript
    print("Attempting to click Quit menu item via AppleScript...")

    daemon_name = get_name_of_bin(daemon_bin)

    applescript_cmd = f"""
    tell application "System Events"
        try
            tell process "{daemon_name}"
                -- Menu bar items behavior varies. Try detecting where it exists.
                if exists (menu bar item 1 of menu bar 1) then
                     set statusItem to first menu bar item of menu bar 1
                else
                     set statusItem to first menu bar item of menu bar 2
                end if

                click statusItem
                delay 0.5
                -- Menu 1 of the status item contains our items
                click menu item "Quit" of menu 1 of statusItem
            end tell
        on error errMsg
            return "ERROR: " & errMsg
        end try
    end tell
    """

    try:
        res = subprocess.run(
            ["osascript", "-e", applescript_cmd],
            check=True,
            capture_output=True,
            text=True,
        )
        output = res.stdout.strip()
        if "ERROR:" in output:
            pytest.fail(f"AppleScript Error: {output}")

    except subprocess.CalledProcessError as e:
        print("Details:", e.output if e.output else e.stderr)
        pytest.fail("AppleScript failed. Check Accessibility permissions.")

    print("Clicked Quit. Waiting for termination...")
    time.sleep(3)

    # 3. Verify Termination
    # Check Daemon
    if daemon_process.poll() is None:
        pytest.fail("Daemon process is still running after Quit.")

    print(f"Daemon terminated successfully (Return code: {daemon_process.returncode}).")

    # Check GUI App
    remaining_apps = get_pids_by_name(APP_NAME)
    if remaining_apps:
        pytest.fail(f"{APP_NAME} child process(es) still running: {remaining_apps}")

    print(f"{APP_NAME} terminated successfully.")
