from unittest.mock import AsyncMock, MagicMock

import pytest
from httpx import ASGITransport, AsyncClient

# Patch lifespan so tests don't spin up motion tasks or real httpx clients
import main as app_module
from core.camera_registry import CameraConfig


@pytest.fixture(autouse=True)
def setup_app_state():
    cameras = {
        "test-cam": CameraConfig(id="test-cam", label="Test Cam", ip="192.168.1.1", enabled=True)
    }
    app_module.app.state.cameras = cameras
    app_module.app.state.sse_subscribers = set()
    app_module.app.state.streamers = {}
    app_module.app.state.api_client = AsyncMock()
    yield


@pytest.fixture
def client():
    return AsyncClient(
        transport=ASGITransport(app=app_module.app),
        base_url="http://test",
    )


async def test_health(client):
    async with client as c:
        resp = await c.get("/health")
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "ok"
    assert data["cameras"] == 1


async def test_root_redirects(client):
    async with client as c:
        resp = await c.get("/", follow_redirects=False)
    assert resp.status_code in (301, 302, 307, 308)
    assert "/ui" in resp.headers["location"]


async def test_list_cameras(client):
    async with client as c:
        resp = await c.get("/api/cameras")
    assert resp.status_code == 200
    cameras = resp.json()
    assert len(cameras) == 1
    assert cameras[0]["id"] == "test-cam"
    assert cameras[0]["stream_url"] == "/api/stream/test-cam"
    assert cameras[0]["snapshot_url"] == "/api/snapshot/test-cam"


async def test_camera_not_found(client):
    async with client as c:
        resp = await c.get("/api/stream/nonexistent")
    assert resp.status_code == 404


async def test_snapshot_camera_offline(client):
    import httpx as _httpx

    app_module.app.state.api_client.get = AsyncMock(side_effect=_httpx.ConnectError("offline"))
    async with client as c:
        resp = await c.get("/api/snapshot/test-cam")
    assert resp.status_code == 503


async def test_snapshot_returned(client):
    import httpx

    fake_jpeg = b"\xff\xd8\xff\xe0fake_jpeg_bytes\xff\xd9"
    mock_resp = MagicMock(spec=httpx.Response)
    mock_resp.content = fake_jpeg
    mock_resp.raise_for_status = MagicMock()
    app_module.app.state.api_client.get = AsyncMock(return_value=mock_resp)

    async with client as c:
        resp = await c.get("/api/snapshot/test-cam")
    assert resp.status_code == 200
    assert resp.headers["content-type"] == "image/jpeg"
    assert resp.content == fake_jpeg


async def test_control_camera_offline(client):
    import httpx as _httpx

    app_module.app.state.api_client.get = AsyncMock(side_effect=_httpx.ConnectError("offline"))

    async with client as c:
        resp = await c.post(
            "/api/cameras/test-cam/control",
            json={"variable": "framesize", "value": 8},
        )
    assert resp.status_code == 503


async def test_control_camera_ok(client):
    import httpx

    mock_resp = MagicMock(spec=httpx.Response)
    mock_resp.raise_for_status = MagicMock()
    app_module.app.state.api_client.get = AsyncMock(return_value=mock_resp)

    async with client as c:
        resp = await c.post(
            "/api/cameras/test-cam/control",
            json={"variable": "brightness", "value": 1},
        )
    assert resp.status_code == 200
    body = resp.json()
    assert body["variable"] == "brightness"
    assert body["value"] == 1
