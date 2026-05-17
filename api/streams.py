import asyncio
import logging
from datetime import datetime
from pathlib import Path

import httpx
from fastapi import APIRouter, HTTPException, Request, Response
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel

from config import settings
from core.broadcaster import CameraStreamer
from core.camera_registry import get_camera

logger = logging.getLogger(__name__)

router = APIRouter()


class ControlCommand(BaseModel):
    variable: str
    value: int


@router.get("/cameras")
async def list_cameras(request: Request):
    cameras = request.app.state.cameras
    return [
        {
            "id": cam.id,
            "label": cam.label,
            "stream_url": f"/api/stream/{cam.id}",
            "snapshot_url": f"/api/snapshot/{cam.id}",
        }
        for cam in cameras.values()
    ]


@router.get("/stream/{cam_id}")
async def stream(cam_id: str, request: Request):
    get_camera(request.app.state.cameras, cam_id)  # raises 404 if unknown
    streamer: CameraStreamer = request.app.state.streamers[cam_id]
    q = streamer.subscribe(maxsize=1)

    async def generator():
        try:
            while True:
                jpeg = await q.get()
                yield (
                    b"\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    + str(len(jpeg)).encode()
                    + b"\r\n\r\n"
                )
                yield jpeg  # no copy — JPEG bytes referenced directly from the queue
        except asyncio.CancelledError:
            return
        finally:
            streamer.unsubscribe(q)

    return StreamingResponse(
        generator(),
        media_type="multipart/x-mixed-replace; boundary=frame",
    )


@router.get("/snapshot/{cam_id}")
async def snapshot(cam_id: str, request: Request, save: bool = False):
    camera = get_camera(request.app.state.cameras, cam_id)
    http_client: httpx.AsyncClient = request.app.state.api_client

    try:
        resp = await http_client.get(camera.capture_url, timeout=settings.proxy_timeout)
        resp.raise_for_status()
    except (httpx.TransportError, httpx.HTTPStatusError):
        raise HTTPException(status_code=503, detail="Camera offline")

    if save:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = Path(settings.snapshots_dir) / f"{cam_id}_{ts}.jpg"
        path.write_bytes(resp.content)

    return Response(content=resp.content, media_type="image/jpeg")


@router.get("/snapshots")
async def list_snapshots():
    snapshots_dir = Path(settings.snapshots_dir)
    files = sorted(snapshots_dir.glob("*.jpg"), key=lambda p: p.stat().st_mtime, reverse=True)
    return [{"filename": f.name, "url": f"/api/snapshots/{f.name}"} for f in files]


@router.get("/snapshots/{filename}")
async def get_snapshot(filename: str):
    path = Path(settings.snapshots_dir) / filename
    if not path.exists() or path.suffix != ".jpg":
        raise HTTPException(status_code=404, detail="Snapshot not found")
    return FileResponse(path, media_type="image/jpeg")


@router.post("/cameras/{cam_id}/control")
async def control_camera(cam_id: str, cmd: ControlCommand, request: Request):
    camera = get_camera(request.app.state.cameras, cam_id)
    http_client: httpx.AsyncClient = request.app.state.api_client

    try:
        await http_client.get(
            camera.control_url,
            params={"var": cmd.variable, "val": cmd.value},
            timeout=15.0,
        )
    except (httpx.TransportError, httpx.HTTPStatusError) as exc:
        logger.warning("Control failed for %s (%s=%s): %s", cam_id, cmd.variable, cmd.value, exc)
        raise HTTPException(status_code=503, detail="Camera offline")

    return {"status": "ok", "camera": cam_id, "variable": cmd.variable, "value": cmd.value}
