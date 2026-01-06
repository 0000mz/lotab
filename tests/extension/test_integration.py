import os
import json
import asyncio
import pytest
import websockets
import pytest_asyncio
from playwright.async_api import async_playwright

# Path to the extension directory
EXTENSION_PATH = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../../extension")
)
WS_PORT = 9001


@pytest.fixture
def extension_path():
    if not os.path.exists(EXTENSION_PATH):
        pytest.fail(f"Extension path does not exist: {EXTENSION_PATH}")
    return EXTENSION_PATH


@pytest_asyncio.fixture
async def mock_server():
    """Starts a mock WebSocket server and yields a queue for connected clients."""
    connected_clients = asyncio.Queue()

    async def handler(websocket):
        print("[Server] Client connected")
        await connected_clients.put(websocket)
        try:
            await websocket.wait_closed()
        except Exception as e:
            print(f"[Server] Connection error: {e}")
        finally:
            print("[Server] Client disconnected")

    async with websockets.serve(handler, "localhost", WS_PORT):
        print(f"[Server] Started on port {WS_PORT}")
        yield connected_clients


@pytest_asyncio.fixture
async def browser_context(extension_path):
    """Launches Chromium with the extension loaded."""
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

        # Launching persistent context is often required for extensions
        try:
            context = await p.chromium.launch_persistent_context(
                user_data_dir,
                args=args,
                headless=False,  # Match original test config
            )

            yield context

            await context.close()
        finally:
            shutil.rmtree(user_data_dir, ignore_errors=True)


@pytest.mark.asyncio
async def test_extension_communication(mock_server, browser_context):
    """Verifies the extension connects and responds to requests."""

    # Warup/Wait: Give the extension a moment to start up and connect
    print("Waiting for extension to connect...")
    try:
        # Wait for the client to connect to our mock server
        client_ws = await asyncio.wait_for(mock_server.get(), timeout=10.0)
    except asyncio.TimeoutError:
        pytest.fail("Extension failed to connect to WebSocket server within timeout.")

    print("Extension connected!")

    # Prepare request
    request = {"event": "Daemon::WS::AllTabsInfoRequest"}

    # Send request
    print(f"Sending: {request}")
    await client_ws.send(json.dumps(request))

    # Wait for response
    print("Waiting for response...")
    try:
        response_str = await asyncio.wait_for(client_ws.recv(), timeout=5.0)
        response = json.loads(response_str)
        print(f"Received: {response}")

        assert response.get("event") == "Extension::WS::AllTabsInfoResponse"
        assert "data" in response
        data = response["data"]
        assert isinstance(data, dict)
        assert "tabs" in data
        assert isinstance(data["tabs"], list)
        assert "groups" in data
        assert isinstance(data["groups"], list)

        print(
            f"Verified {len(data['tabs'])} tabs and {len(data['groups'])} groups received."
        )

    except asyncio.TimeoutError:
        pytest.fail("Timed out waiting for response from extension.")
    except json.JSONDecodeError:
        pytest.fail(f"Received invalid JSON: {response_str}")
