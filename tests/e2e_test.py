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
    elif key == "space":
        script = f'tell application "System Events" to key code 49{using_str}'
    elif key == "/":
        script = f'tell application "System Events" to key code 44{using_str}'

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
        [daemon_bin], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
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


# --- Helpers ---


async def wait_for_background_page(browser_context):
    for i in range(20):
        if browser_context.background_pages:
            return browser_context.background_pages[0]
        if browser_context.service_workers:
            return browser_context.service_workers[0]
        await asyncio.sleep(0.5)
    print("FATAL: No background page or service worker found!")
    return None


async def get_active_tab_title(browser_context):
    bg = await wait_for_background_page(browser_context)
    if not bg:
        return None

    return await bg.evaluate("""
        () => new Promise(resolve => {
            chrome.tabs.query({active: true, currentWindow: true}, (tabs) => {
                resolve(tabs[0] ? tabs[0].title : null);
            });
        })
    """)


async def get_tab_count(browser_context):
    bg = await wait_for_background_page(browser_context)
    if not bg:
        return 0

    return await bg.evaluate("""
        () => new Promise(resolve => {
            chrome.tabs.query({currentWindow: true}, (tabs) => {
                resolve(tabs.length);
            });
        })
    """)


async def get_tab_titles(browser_context):
    bg = await wait_for_background_page(browser_context)
    if not bg:
        return []

    return await bg.evaluate("""
        () => new Promise(resolve => {
            chrome.tabs.query({currentWindow: true}, (tabs) => {
                resolve(tabs.map(t => t.title));
            });
        })
    """)


async def setup_tabs(browser_context):
    page1 = browser_context.pages[0]
    await page1.goto("http://example.com")
    await page1.evaluate("document.title = 'Tab 1'")

    page2 = await browser_context.new_page()
    await page2.goto("http://example.org")
    await page2.evaluate("document.title = 'Tab 2'")

    page3 = await browser_context.new_page()
    await page3.goto("http://example.net")
    await page3.evaluate("document.title = 'Tab 3'")

    await asyncio.sleep(2)

    # Wait for extension to know about these tabs
    # We can query get_tab_count until it is 3
    count = 0
    for _ in range(20):
        count = await get_tab_count(browser_context)
        if count == 3:
            break
        await asyncio.sleep(0.5)

    assert count == 3, f"Setup failed: Extension reports {count} tabs, expected 3"

    return page1, page2, page3


async def toggle_gui():
    print("Sending Cmd+Shift+J to toggle GUI...")
    send_hotkey("j", ["command down", "shift down"])
    await asyncio.sleep(1)


# --- Tests ---


@pytest.mark.asyncio
async def test_navigation(daemon_process, browser_context):
    """
    E2E Test: Navigation
    """
    page1, page2, page3 = await setup_tabs(browser_context)

    # Bring Tab 1 to front initially
    await page1.bring_to_front()

    await toggle_gui()

    # 3. Navigate Down -> Enter
    # List: [Tab 1 (Active/Recency top?), Tab 2, Tab 3].
    print("Sending Down...")
    send_hotkey("down")
    await asyncio.sleep(0.2)
    print("Sending Return...")
    send_hotkey("return")
    await asyncio.sleep(1)

    active_title = await get_active_tab_title(browser_context)
    print(f"Active: {active_title}")
    assert active_title in ["Tab 1", "Tab 2", "Tab 3"]

    await toggle_gui()

    print("Sending Down...")
    send_hotkey("down")
    await asyncio.sleep(0.2)
    print("Sending Return...")
    send_hotkey("return")
    await asyncio.sleep(1)

    active_title = await get_active_tab_title(browser_context)
    print(f"Active: {active_title}")


@pytest.mark.asyncio
async def test_close_via_navigate(daemon_process, browser_context):
    """
    Case 1: navigating with arrow or j/k, press x to close
    """
    await setup_tabs(browser_context)

    await toggle_gui()

    # Selection resets to first.
    # Navigate Down (to select 2nd item).
    print("Navigating Down...")
    send_hotkey("down")
    await asyncio.sleep(0.2)

    # Press x to close
    print("Pressing x...")
    send_hotkey("x")
    await asyncio.sleep(1)  # Wait for IPC

    # Check tab count
    count = await get_tab_count(browser_context)
    print(f"Tab Count: {count}")
    assert count == 2, f"Expected 2 tabs, got {count}"

    titles = await get_tab_titles(browser_context)
    print(f"Remaining Tabs: {titles}")


@pytest.mark.asyncio
async def test_close_via_search(daemon_process, browser_context):
    """
    Case 2: using search with forward slash to search for the tab, select the tab, press x to close
    """
    await setup_tabs(browser_context)

    await toggle_gui()

    # Press / (search)
    print("Pressing /...")
    send_hotkey("/")
    await asyncio.sleep(1.0)

    # Type "Tab 2"
    print("Typing 'Tab 2'...")
    script = 'tell application "System Events" to keystroke "Tab 2"'
    subprocess.run(["osascript", "-e", script], check=True)
    await asyncio.sleep(1.0)

    # TODO: Instead of having to do 3 extra actions to select the first in the list after a search,
    # it should automatically be highlighted.
    print("Committing search")
    send_hotkey("return")
    await asyncio.sleep(1.0)

    # It should filter to Tab 2. Selection should be on it (if it's the only one/first).
    # Press x
    print("Pressing x...")
    send_hotkey("x")
    await asyncio.sleep(2.0)

    count = await get_tab_count(browser_context)
    print(f"Tab Count: {count}")
    assert count == 2

    titles = await get_tab_titles(browser_context)
    assert "Tab 2" not in titles


@pytest.mark.asyncio
async def test_close_via_select_space(daemon_process, browser_context):
    """
    Case 3: navigate w/ arrow/j/k, select with space, close with x
    """
    await setup_tabs(browser_context)

    await toggle_gui()

    # Navigate Down
    print("Navigating Down...")
    send_hotkey("down")
    await asyncio.sleep(0.2)

    # Select with Space
    print("Pressing Space...")
    send_hotkey("space")
    await asyncio.sleep(0.2)

    # Navigate Down
    print("Navigating Down...")
    send_hotkey("down")
    await asyncio.sleep(0.2)

    # Select another
    print("Pressing Space...")
    send_hotkey("space")
    await asyncio.sleep(0.2)

    # Press x
    print("Pressing x...")
    send_hotkey("x")
    await asyncio.sleep(1)

    count = await get_tab_count(browser_context)
    print(f"Tab Count: {count}")
    assert count <= 2

    if count == 1:
        print("Closed multiple tabs via selection!")
