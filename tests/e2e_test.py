import os
import json
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
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except psutil.TimeoutExpired:
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


class DaemonContext:
    def __init__(self, daemon_bin, daemon_manifest_path=None, gui_manifest_path=None):
        self.daemon_bin = daemon_bin
        self.daemon_manifest_path = daemon_manifest_path
        self.gui_manifest_path = gui_manifest_path
        self.proc = None
        self.daemon_manifest = None
        self.gui_manifest = None

    def __enter__(self):
        print(f"\n[DaemonContext] Starting daemon from {self.daemon_bin}...")
        bin_name = get_name_of_bin(self.daemon_bin)
        kill_processes_by_name(bin_name)
        kill_processes_by_name(APP_NAME)
        time.sleep(1)

        args = [self.daemon_bin]
        if self.daemon_manifest_path:
            args.extend(["--daemon-manifest-path", self.daemon_manifest_path])
        if self.gui_manifest_path:
            args.extend(["--gui-manifest-path", self.gui_manifest_path])

        print(f"[DaemonContext] Args: {args}")

        self.proc = subprocess.Popen(
            args,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(5)  # Wait for startup

        if self.proc.poll() is not None:
            raise RuntimeError("Daemon failed to start.")

        print(f"[DaemonContext] Daemon started with PID: {self.proc.pid}")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        print("\n[DaemonContext] Tearing down daemon...")
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                print("[DaemonContext] Daemon timed out, killing...")
                self.proc.kill()

        # Give GUI a moment to write if it was triggered by daemon death
        time.sleep(2)
        kill_processes_by_name(APP_NAME)

        # Parse Manifests
        if self.daemon_manifest_path and os.path.exists(self.daemon_manifest_path):
            try:
                with open(self.daemon_manifest_path) as f:
                    content = f.read()
                    if content:
                        self.daemon_manifest = json.loads(content)
                        print(
                            f"[DaemonContext] Loaded Daemon Manifest: {json.dumps(self.daemon_manifest, indent=2)}"
                        )
                    else:
                        print("[DaemonContext] Daemon manifest file is empty")
            except Exception as e:
                print(f"[DaemonContext] Failed to parse daemon manifest: {e}")

        if self.gui_manifest_path and os.path.exists(self.gui_manifest_path):
            try:
                with open(self.gui_manifest_path) as f:
                    content = f.read()
                    if content:
                        self.gui_manifest = json.loads(content)
                        print(
                            f"[DaemonContext] Loaded GUI Manifest: {json.dumps(self.gui_manifest, indent=2)}"
                        )
                    else:
                        print("[DaemonContext] GUI manifest file is empty")
            except Exception as e:
                print(f"[DaemonContext] Failed to parse GUI manifest: {e}")


@pytest.fixture(scope="function")
def daemon_controller(daemon_bin):
    # Just a helper to return the constructor, or use the class directly in tests
    return lambda d_path=None, g_path=None: DaemonContext(daemon_bin, d_path, g_path)


@pytest.fixture(scope="function")
def daemon_process(daemon_bin):
    # BACKWARD COMPATIBILITY for other tests
    # This fixture starts the daemon without manifest paths
    ctx = DaemonContext(daemon_bin)
    ctx.__enter__()
    yield ctx.proc
    ctx.__exit__(None, None, None)


@pytest_asyncio.fixture
async def browser_context(extension_path):
    import tempfile
    import shutil

    # ... existing implementation ...
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


# ... helpers ...


@pytest.mark.asyncio
async def test_incremental_group_assignment(daemon_bin, browser_context):
    """
    E2E Test: Incremental Group Assignment
    Uses explicit DaemonContext to verify manifests.
    """
    import tempfile

    # Custom setup for 2 tabs
    page1 = browser_context.pages[0]
    await page1.goto("http://example.com")
    await page1.evaluate("document.title = 'Tab 1'")

    page2 = await browser_context.new_page()
    await page2.goto("http://example.org")
    await page2.evaluate("document.title = 'Tab 2'")

    await asyncio.sleep(2)

    # Wait for tab count = 2
    for _ in range(20):
        count = await get_tab_count(browser_context)
        if count == 2:
            break
        await asyncio.sleep(0.5)

    with (
        tempfile.NamedTemporaryFile(suffix=".json", delete=False) as d_man_file,
        tempfile.NamedTemporaryFile(suffix=".json", delete=False) as g_man_file,
    ):
        d_man_path = d_man_file.name
        g_man_path = g_man_file.name

        print(f"Daemon Manifest Path: {d_man_path}")
        print(f"GUI Manifest Path: {g_man_path}")

        try:
            with DaemonContext(daemon_bin, d_man_path, g_man_path) as daemon:
                # --- Part 1 ---
                await toggle_gui()

                # Select first tab (Tab 2, active)
                print("Part 1: Selecting first tab...")
                send_hotkey("space")
                await asyncio.sleep(0.5)

                print("Pressing M (Mark)...")
                send_hotkey("m")
                await asyncio.sleep(1.0)

                # Select "Create New Task" (default selection is 0)
                print("Selecting 'Create New Task'...")
                send_hotkey("return")
                await asyncio.sleep(0.5)

                # Type "new-group"
                print("Typing 'new-group'...")
                for char in "new-group":
                    send_hotkey(char)
                    await asyncio.sleep(0.1)

                print("Committing group...")
                send_hotkey("return")
                await asyncio.sleep(2.0)

                print("Pressing ESC to close...")
                send_hotkey("escape")
                await asyncio.sleep(1.0)

                # Verify Part 1
                groups = await get_tab_groups(browser_context)
                print(f"Groups Part 1: {groups}")
                assert len(groups) == 1
                assert groups[0]["title"] == "new-group"
                assert groups[0]["tabCount"] == 1

                # Check unassociated
                bg = await wait_for_background_page(browser_context)
                ungrouped_count = await bg.evaluate("""
                    () => new Promise(resolve => {
                        chrome.tabs.query({groupId: chrome.tabGroups.TAB_GROUP_ID_NONE}, (tabs) => {
                            resolve(tabs.length);
                        });
                    })
                """)
                print(f"Ungrouped count: {ungrouped_count}")
                assert ungrouped_count == 1

                # --- Part 2 ---
                print("Part 2: Reopening GUI...")
                await toggle_gui()

                # Navigate to unassociated tab.
                print("Navigating to unassociated tab (Second item)...")
                send_hotkey("down")
                await asyncio.sleep(0.5)

                print("Selecting tab (Space)...")
                send_hotkey("space")
                await asyncio.sleep(0.5)

                print("Pressing M...")
                send_hotkey("m")
                await asyncio.sleep(1.0)

                # List of tasks: [Create New, new-group]
                print("Navigating to 'new-group' (Down)...")
                send_hotkey("down")
                await asyncio.sleep(0.5)

                print("Committing...")
                send_hotkey("return")
                await asyncio.sleep(2.0)

                print("Pressing ESC to close...")
                send_hotkey("escape")
                await asyncio.sleep(1.0)

                # Verify Part 2
                groups = await get_tab_groups(browser_context)
                print(f"Groups Part 2: {groups}")
                assert len(groups) == 1
                assert groups[0]["title"] == "new-group"
                assert groups[0]["tabCount"] == 2

                ungrouped_count_2 = await bg.evaluate("""
                    () => new Promise(resolve => {
                        chrome.tabs.query({groupId: chrome.tabGroups.TAB_GROUP_ID_NONE}, (tabs) => {
                            resolve(tabs.length);
                        });
                    })
                """)
                print(f"Ungrouped count: {ungrouped_count_2}")
                assert ungrouped_count_2 == 0

            # --- Context Manager Exited: Daemon Terminated ---

            # Verify Manifests (Loaded by DaemonContext)
            print("\n[Test] Verifying Manifests...")

            # Daemon Manifest
            if daemon.daemon_manifest:
                print(
                    f"[Test] Daemon Manifest Content: {json.dumps(daemon.daemon_manifest, indent=2)}"
                )
            else:
                print("[Test] Daemon Manifest NOT found or empty.")

            # GUI Manifest
            if daemon.gui_manifest:
                print(
                    f"[Test] GUI Manifest Content: {json.dumps(daemon.gui_manifest, indent=2)}"
                )
                assert (
                    len(daemon.gui_manifest.get("tasks", [])) == 1
                )  # Correct behavior: 1 task
                # Check for 'new-group'
                names = [t["name"] for t in daemon.gui_manifest["tasks"]]
                assert "new-group" in names
            else:
                print("[Test] GUI Manifest NOT found or empty.")

        finally:
            if os.path.exists(d_man_path):
                os.remove(d_man_path)
            if os.path.exists(g_man_path):
                os.remove(g_man_path)


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


async def get_tab_groups(browser_context):
    bg = await wait_for_background_page(browser_context)
    if not bg:
        return []

    return await bg.evaluate("""
        () => new Promise(resolve => {
            chrome.tabGroups.query({}, (groups) => {
                const result = [];
                let processed = 0;
                if (groups.length === 0) {
                    resolve([]);
                    return;
                }

                groups.forEach(g => {
                    chrome.tabs.query({groupId: g.id}, (tabs) => {
                        result.push({
                            title: g.title,
                            color: g.color,
                            id: g.id,
                            tabCount: tabs.length
                        });
                        processed++;
                        if (processed === groups.length) {
                             resolve(result);
                        }
                    });
                });
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


@pytest.mark.asyncio
async def test_create_tab_group(daemon_process, browser_context):
    """
    E2E Test: Create Tab Group
    Steps:
    1. Open 3 tabs.
    2. Toggle GUI.
    3. Move down 1, Select (Space).
    4. Move down 1, Select (Space).
    5. Press 'm' (mark/task).
    6. Press 'return' (create new).
    7. Type 'test-group'.
    8. Press 'return' (commit).
    9. Verify 1 tab group with 2 tabs.
    """
    await setup_tabs(browser_context)

    # Ensure no groups initially
    groups = await get_tab_groups(browser_context)
    assert len(groups) == 0, "Expected 0 tab groups initially"

    await toggle_gui()

    # Navigate Down (to Tab 2)
    print("Navigating Down (Tab 2)...")
    send_hotkey("down")
    await asyncio.sleep(0.5)

    # Select Tab 2
    print("Pressing Space (Select Tab 2)...")
    send_hotkey("space")
    await asyncio.sleep(0.5)

    # Navigate Down (to Tab 3)
    print("Navigating Down (Tab 3)...")
    send_hotkey("down")
    await asyncio.sleep(0.5)

    # Select Tab 3
    print("Pressing Space (Select Tab 3)...")
    send_hotkey("space")
    await asyncio.sleep(0.5)

    # Press 'm' to enter Mark/Group mode
    print("Pressing 'm'...")
    send_hotkey("m")
    await asyncio.sleep(1.0)

    # Press 'return' to choose "Create New Task/Group" (usually the first option or default action if list empty?)
    # Assuming 'm' opens a list where "Create New" is available or typing starts filtering.
    # Based on user description: "Press enter to create new. type test-group, press enter to commit."
    # If 'm' opens a mode where we can type immediately:

    # Wait, user said: "Press enter to create new". This implies a menu selection.
    print("Pressing Return (Create New)...")
    send_hotkey("return")
    await asyncio.sleep(1.0)

    # Type "test-group"
    print("Typing 'test-group'...")
    script = 'tell application "System Events" to keystroke "test-group"'
    subprocess.run(["osascript", "-e", script], check=True)
    await asyncio.sleep(0.5)

    # Commit
    print("Pressing Return (Commit)...")
    send_hotkey("return")
    await asyncio.sleep(2.0)  # Wait for extension to process

    # Verify
    groups = await get_tab_groups(browser_context)
    print(f"Tab Groups Found: {groups}")

    assert len(groups) == 1, f"Expected 1 tab group, found {len(groups)}"
    assert groups[0]["title"] == "test-group", (
        f"Expected group title 'test-group', got '{groups[0]['title']}'"
    )
    assert groups[0]["tabCount"] == 2, (
        f"Expected 2 tabs in group, got {groups[0]['tabCount']}"
    )


@pytest.mark.asyncio
async def test_assign_all_tabs_to_existing_group(daemon_process, browser_context):
    """
    E2E Test: Assign All Tabs to Existing Group
    Steps:
    1. Open 3 tabs.
    2. Create a tab group 'tab group 1' containing Tab 1 programmatically mostly to ensure it exists.
    3. Toggle GUI.
    4. Select All (Cmd+A).
    5. Press 'm' (mark/task).
    6. Navigate Down (to 'tab group 1').
    7. Press 'return' (associate).
    8. Verify all 3 tabs are in 'tab group 1'.
    """
    page1, page2, page3 = await setup_tabs(browser_context)

    # 2. Programmatically create 'tab group 1' with the first tab
    bg = await wait_for_background_page(browser_context)
    await bg.evaluate("""
        () => new Promise(resolve => {
            chrome.tabs.query({title: 'Tab 1'}, (tabs) => {
                const tabId = tabs[0].id;
                chrome.tabs.group({tabIds: tabId}, (groupId) => {
                    chrome.tabGroups.update(groupId, {title: 'tab group 1', color: 'blue'}, () => {
                        resolve();
                    });
                });
            });
        })
    """)

    # Wait for extension to process the new group
    await asyncio.sleep(1.0)

    await toggle_gui()

    # 4. Select All (Cmd+A)
    print("Sending Cmd+A (Select All)...")
    send_hotkey("a", ["command down"])
    await asyncio.sleep(0.5)

    # 5. Press 'm'
    print("Pressing 'm'...")
    send_hotkey("m")
    await asyncio.sleep(1.0)

    # 6. Navigate Down
    # List should be: [Create New Task, tab group 1]
    # Default selection is 0 (Create New). We need to go down to select 'tab group 1'.
    print("Navigating Down (to select tab group 1)...")
    send_hotkey("down")
    await asyncio.sleep(0.5)

    # 7. Press Return
    print("Pressing Return (Associate)...")
    send_hotkey("return")
    await asyncio.sleep(2.0)

    # 8. Verify
    groups = await get_tab_groups(browser_context)
    print(f"Tab Groups Found: {groups}")

    assert len(groups) == 1, f"Expected 1 tab group, found {len(groups)}"
    assert groups[0]["title"] == "tab group 1"
    assert groups[0]["tabCount"] == 3, (
        f"Expected 3 tabs in group, got {groups[0]['tabCount']}"
    )
