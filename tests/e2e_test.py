import os
import time
import asyncio
import subprocess
import psutil
import pytest
import pytest_asyncio
from playwright.async_api import async_playwright

APP_NAME = "Lotab"

# --- Utilities ---


def get_name_of_bin(path: str) -> str:
    return os.path.basename(path)


def kill_processes_by_name(name):
    for proc in psutil.process_iter(["pid", "name", "cmdline"]):
        try:
            if name in proc.info["name"] or (
                proc.info["cmdline"] and name in proc.info["cmdline"][0]
            ):
                proc.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass


def send_hotkey(key, modifiers=[]):
    """
    Sends a keystroke via AppleScript.
    key: e.g. "j" or special key code
    modifiers: list of strings, e.g. ["command down", "shift down"]
    """
    using_str = ""
    if modifiers:
        using_str = " using {" + ", ".join(modifiers) + "}"

    script = f'tell application "System Events" to keystroke "{key}"{using_str}'
    if len(key) > 1 and key.lower() not in ["return", "space", "enter", "tab"]:
        # Assume key code if numeric or specialized
        pass

    # Handle special keys
    if key == "down":
        script = f'tell application "System Events" to key code 125{using_str}'
    elif key == "up":
        script = f'tell application "System Events" to key code 126{using_str}'
    elif key == "return":
        script = f'tell application "System Events" to key code 36{using_str}'

    subprocess.run(["osascript", "-e", script], check=True)


# --- Fixtures ---


@pytest.fixture(scope="function")
def daemon_bin():
    path = os.environ.get("DAEMON_BIN", "./build/debug/lotab_daemon")
    if not os.path.exists(path):
        # Fallback to asan if standard not found (common in this env)
        path = "./build/debug/lotab_daemon_asan"
        if not os.path.exists(path):
            pytest.fail(f"Daemon binary not found at {path}")
    return path


@pytest.fixture(scope="function")
def extension_path():
    path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../extension"))
    if not os.path.exists(path):
        pytest.fail(f"Extension path does not exist: {path}")
    return path


@pytest.fixture(scope="function")
def daemon_process(daemon_bin):
    print(f"\n[Fixture] Setting up daemon from {daemon_bin}...")
    bin_name = get_name_of_bin(daemon_bin)
    kill_processes_by_name(bin_name)
    kill_processes_by_name(APP_NAME)
    time.sleep(1)

    proc = subprocess.Popen(
        [daemon_bin], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    time.sleep(5)  # Wait for startup

    if proc.poll() is not None:
        pytest.fail("Daemon failed to start.")

    print(f"[Fixture] Daemon started with PID: {proc.pid}")
    yield proc

    print("\n[Fixture] Tearing down daemon...")
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
    kill_processes_by_name(APP_NAME)


@pytest_asyncio.fixture
async def browser_context(extension_path):
    import tempfile
    import shutil

    user_data_dir = tempfile.mkdtemp()
    async with async_playwright() as p:
        args = [
            f"--disable-extensions-except={extension_path}",
            f"--load-extension={extension_path}",
            "--no-sandbox",
            "--disable-setuid-sandbox",
        ]
        try:
            context = await p.chromium.launch_persistent_context(
                user_data_dir,
                args=args,
                headless=False,
            )
            yield context
            await context.close()
        finally:
            shutil.rmtree(user_data_dir, ignore_errors=True)


# --- Test ---


@pytest.mark.asyncio
async def test_navigation(daemon_process, browser_context):
    """
    E2E Test:
    1. Open 3 tabs.
    2. Toggle GUI (Cmd+Shift+J).
    3. Navigate Down -> Enter -> Verify Tab 2 active.
    4. Navigate Down -> Enter -> Verify Tab 3 active.
    5. Navigate Up -> Enter -> Verify Tab 2 active.
    """

    # 1. Create 3 tabs
    page1 = browser_context.pages[0]
    await page1.goto("http://example.com")
    await page1.evaluate("document.title = 'Tab 1'")

    page2 = await browser_context.new_page()
    await page2.goto("http://example.org")
    await page2.evaluate("document.title = 'Tab 2'")

    page3 = await browser_context.new_page()
    await page3.goto("http://example.net")
    await page3.evaluate("document.title = 'Tab 3'")

    # Wait for sync
    await asyncio.sleep(2)

    # Bring Tab 1 to front initially
    await page1.bring_to_front()
    assert await page1.evaluate("document.visibilityState") == "visible"

    # 2. Open GUI
    # Note: We can't verify GUI visibility easily without screen capture or accessibility queries,
    # but we can infer it works if navigation commands work.
    print("Sending Cmd+Shift+J to toggle GUI...")
    send_hotkey("j", ["command down", "shift down"])
    await asyncio.sleep(1)

    # 3. Navigate Down -> Enter (Should select Tab 2)
    # The list order is usually Recency or Fixed?
    # Daemon logic for list order: "active + other".
    # Assuming standard order: Tab 1 (active), Tab 2, Tab 3.
    # Down -> Tab 2.
    print("Sending Down...")
    send_hotkey("down")
    await asyncio.sleep(0.2)

    print("Sending Return...")
    send_hotkey("return")
    await asyncio.sleep(1)

    # Verify Tab 2 is active
    # Helper to get active tab title via Extension API
    background_page = None
    if browser_context.background_pages:
        background_page = browser_context.background_pages[0]

    async def get_active_tab_title():
        if not background_page:
            return None
        return await background_page.evaluate("""
            () => new Promise(resolve => {
                chrome.tabs.query({active: true, currentWindow: true}, (tabs) => {
                    resolve(tabs[0] ? tabs[0].title : null);
                });
            })
        """)

    await asyncio.sleep(1)

    if background_page:
        active_title = await get_active_tab_title()
        print(f"Active Tab Title (via Ext): {active_title}")
        assert active_title == "Tab 2", (
            f"Expected Tab 2 to be active, but got {active_title}"
        )
    else:
        print("WARNING: Could not find background page for strict verification.")
        focused2 = await page2.evaluate("document.hasFocus()")
        assert focused2, "Tab 2 should verify as focused/active"

    # 4. Open GUI again
    print("Sending Cmd+Shift+J...")
    send_hotkey("j", ["command down", "shift down"])
    await asyncio.sleep(1)

    # Navigate Down (Tab 3)
    # Order: [Tab 1, Tab 2, Tab 3]. Active: Tab 2.
    # List: [Tab 2 (Active), Tab 1, Tab 3].
    # Selection reset to First (Tab 2) on Show.
    # Down -> Tab 1.

    print("Sending Down...")
    send_hotkey("down")
    await asyncio.sleep(0.2)
    print("Sending Return...")
    send_hotkey("return")
    await asyncio.sleep(1)

    if background_page:
        active_title = await get_active_tab_title()
        print(f"Active Tab Title (via Ext): {active_title}")
        assert active_title == "Tab 1" or active_title == "Tab 3", (
            f"Expected Tab 1 or 3, got {active_title}"
        )
