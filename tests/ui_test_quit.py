# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "psutil",
# ]
# ///

import subprocess
import time
import os
import sys
import psutil

DAEMON_BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/daemon"
APP_NAME = "Lotab"

def get_pids_by_name(name):
    pids = []
    for proc in psutil.process_iter(['pid', 'name', 'cmdline']):
        try:
            # Match if name is in process name OR if it's in the command line (for daemon arg 0)
            if name in proc.info['name'] or (proc.info['cmdline'] and name in proc.info['cmdline'][0]):
                pids.append(proc.info['pid'])
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass
    return pids

def kill_proceses_by_name(name):
    for proc in psutil.process_iter(['pid', 'name', 'cmdline']):
        try:
            if name in proc.info['name'] or (proc.info['cmdline'] and name in proc.info['cmdline'][0]):
                proc.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass

def test_quit_via_menu():
    print("--- Starting Test: Quit via Menu (uv managed) ---")

    # 1. Cleanup previous instances
    kill_proceses_by_name("daemon")
    kill_proceses_by_name("TabManager")
    time.sleep(1)

    # 2. Build Check
    if not os.path.exists(DAEMON_BIN):
        print(f"Error: {DAEMON_BIN} not found. Run ./build.sh first.")
        sys.exit(1)

    # 3. Start Daemon
    print(f"Launching {DAEMON_BIN}...")
    daemon_proc = subprocess.Popen([DAEMON_BIN], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # Wait for startup
    # Increased wait time to ensure status item is fully registered
    time.sleep(5)

    if daemon_proc.poll() is not None:
        print("ERROR: Daemon failed to start or exited immediately.")
        sys.exit(1)

    print("Daemon running with PID:", daemon_proc.pid)

    # 4. Verify Child App Spawned
    # Using psutil we can be more specific, but get_pids_by_name is reused
    app_pids = get_pids_by_name("TabManager")

    # Filter out the daemon itself if it got matched (unlikely with "TabManager" vs "daemon", but safe)
    app_pids = [p for p in app_pids if p != daemon_proc.pid]

    if not app_pids:
        print("ERROR: TabManager GUI did not spawn.")
        daemon_proc.terminate()
        sys.exit(1)

    print("TabManager running with PID(s):", app_pids)

    # 5. Interact with UI via AppleScript
    print("Attempting to click Quit menu item via AppleScript...")
    print("NOTE: This requires Accessibility permissions for the terminal/editor running this script.")

    applescript_cmd = """
    tell application "System Events"
        try
            tell process "daemon"
                -- Menu bar items behavior varies. Try detecting where it exists.
                -- Often agent apps are in menu bar 2, but sometimes menu bar 1.
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
        res = subprocess.run(["osascript", "-e", applescript_cmd], check=True, capture_output=True, text=True)
        output = res.stdout.strip()
        if "ERROR:" in output:
             print(f"AppleScript Error: {output}")
             raise subprocess.CalledProcessError(1, "osascript", output=output)

    except subprocess.CalledProcessError as e:
        print("ERROR: AppleScript failed to interact with the menu.")
        print("Details:", e.output if e.output else e.stderr)
        print("Please check System Settings -> Privacy & Security -> Accessibility.")

        daemon_proc.terminate()
        kill_proceses_by_name("TabManager")
        sys.exit(1)

    print("Clicked Quit. Waiting for termination...")
    time.sleep(3)

    # 6. Verify Termination
    failure = False

    # Check Daemon
    if daemon_proc.poll() is None:
        print("ERROR: Daemon process is still running.")
        daemon_proc.terminate()
        failure = True
    else:
        print(f"Daemon terminated successfully (Return code: {daemon_proc.returncode}).")

    # Check GUI App
    remaining_apps = get_pids_by_name("TabManager")
    if remaining_apps:
        print(f"ERROR: TabManager child process(es) still running: {remaining_apps}")
        failure = True
    else:
        print("TabManager terminated successfully.")

    if failure:
        sys.exit(1)

    print("SUCCESS: Quit via menu worked correctly.")

if __name__ == "__main__":
    test_quit_via_menu()
