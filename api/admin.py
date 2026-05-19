import asyncio
import os
import re
import signal
import time
from pathlib import Path

import psutil
from fastapi import APIRouter, HTTPException, Query, Request

from config import settings

router = APIRouter()

_SNAPSHOT_RE = re.compile(r"^(.+)_(\d{8}_\d{6})\.jpg$")


def _cpu_temp() -> float | None:
    try:
        temps = psutil.sensors_temperatures()
    except AttributeError:
        return None
    for key in ("cpu-thermal", "cpu_thermal", "coretemp", "acpitz"):
        if key in temps and temps[key]:
            return round(temps[key][0].current, 1)
    if temps:
        first = next(iter(temps.values()))
        if first:
            return round(first[0].current, 1)
    return None


@router.get("/system")
async def system_metrics():
    snapshots_dir = Path(settings.snapshots_dir)
    snapshot_files = list(snapshots_dir.glob("*.jpg"))
    snapshot_size_mb = round(sum(f.stat().st_size for f in snapshot_files) / 1_048_576, 2)

    ram = psutil.virtual_memory()
    swap = psutil.swap_memory()
    disk = psutil.disk_usage("/")

    return {
        "cpu_percent": psutil.cpu_percent(interval=0.2),
        "cpu_temp_c": _cpu_temp(),
        "ram_used_mb": round(ram.used / 1_048_576),
        "ram_total_mb": round(ram.total / 1_048_576),
        "ram_percent": ram.percent,
        "swap_used_mb": round(swap.used / 1_048_576),
        "swap_total_mb": round(swap.total / 1_048_576),
        "swap_percent": swap.percent,
        "disk_used_gb": round(disk.used / 1_073_741_824, 1),
        "disk_total_gb": round(disk.total / 1_073_741_824, 1),
        "disk_percent": disk.percent,
        "snapshots_count": len(snapshot_files),
        "snapshots_size_mb": snapshot_size_mb,
        "uptime_seconds": int(time.time() - psutil.boot_time()),
        "load_avg_1m": round(os.getloadavg()[0], 2),
    }


@router.get("/cameras/status")
async def cameras_status(request: Request):
    cameras = request.app.state.cameras
    streamers = request.app.state.streamers
    motion_stats = request.app.state.motion_stats
    api_client = request.app.state.api_client

    async def ping(cam):
        start = time.monotonic()

        async def _capture():
            r = await api_client.get(cam.capture_url, timeout=3.0)
            r.raise_for_status()
            return round((time.monotonic() - start) * 1000)

        async def _rssi():
            r = await api_client.get(cam.wifi_url, timeout=2.0)
            return r.json().get("rssi")

        capture_result, rssi_result = await asyncio.gather(
            _capture(), _rssi(), return_exceptions=True
        )

        reachable  = not isinstance(capture_result, Exception)
        latency_ms = capture_result if reachable else None
        rssi       = rssi_result if not isinstance(rssi_result, Exception) else None

        streamer = streamers.get(cam.id)
        stats = motion_stats.get(cam.id, {})
        return {
            "id": cam.id,
            "label": cam.label,
            "ip": cam.ip,
            "reachable": reachable,
            "latency_ms": latency_ms,
            "rssi": rssi,
            "subscribers": len(streamer._subscribers) if streamer else 0,
            "motion_events_total": stats.get("events_total", 0),
            "motion_last_at": stats.get("last_at"),
        }

    results = await asyncio.gather(*[ping(cam) for cam in cameras.values()])
    return list(results)


@router.get("/snapshots")
async def list_snapshots(
    camera_id: str | None = Query(default=None),
    limit: int = Query(default=100, ge=1, le=1000),
):
    snapshots_dir = Path(settings.snapshots_dir)
    all_files = sorted(snapshots_dir.glob("*.jpg"), key=lambda f: f.stat().st_mtime, reverse=True)
    total_count = len(all_files)
    total_size  = sum(f.stat().st_size for f in all_files)

    items = []
    for f in all_files:
        m = _SNAPSHOT_RE.match(f.name)
        cam_id = m.group(1) if m else None
        ts = m.group(2) if m else None
        if camera_id and cam_id != camera_id:
            continue
        items.append(
            {
                "filename": f.name,
                "camera_id": cam_id,
                "timestamp": ts,
                "size_bytes": f.stat().st_size,
                "url": f"/api/snapshots/{f.name}",
            }
        )
        if len(items) >= limit:
            break

    return {
        "total": total_count,
        "size_mb": round(total_size / 1_048_576, 2),
        "files": items,
    }


@router.delete("/snapshots")
async def delete_snapshots(
    camera_id: str | None = Query(default=None),
    older_than_days: float | None = Query(default=None),
):
    snapshots_dir = Path(settings.snapshots_dir)
    cutoff = time.time() - older_than_days * 86400 if older_than_days is not None else None
    deleted = 0
    for f in snapshots_dir.glob("*.jpg"):
        if camera_id:
            m = _SNAPSHOT_RE.match(f.name)
            if not m or m.group(1) != camera_id:
                continue
        if cutoff is not None and f.stat().st_mtime >= cutoff:
            continue
        f.unlink()
        deleted += 1
    return {"deleted": deleted}


@router.delete("/snapshots/{filename}")
async def delete_snapshot(filename: str):
    if "/" in filename or ".." in filename or not filename.endswith(".jpg"):
        raise HTTPException(status_code=400, detail="Invalid filename")
    f = Path(settings.snapshots_dir) / filename
    if not f.exists():
        raise HTTPException(status_code=404, detail="Snapshot not found")
    f.unlink()
    return {"deleted": 1}


@router.post("/restart")
async def restart_server():
    async def _deferred():
        await asyncio.sleep(1.5)
        os.kill(os.getpid(), signal.SIGTERM)

    asyncio.create_task(_deferred())
    return {"status": "restarting", "message": "Server will restart in ~2 seconds"}
