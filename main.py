import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from api.admin import router as admin_router
from api.events import router as events_router
from api.streams import router as streams_router
from config import settings
from core.broadcaster import CameraStreamer
from core.camera_registry import load_cameras
from core.motion import motion_detection_task

logging.basicConfig(
    level=logging.DEBUG if settings.debug else logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s — %(message)s",
)

logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    Path(settings.snapshots_dir).mkdir(exist_ok=True)

    app.state.cameras = load_cameras(settings.cameras_file)
    app.state.api_client = httpx.AsyncClient()  # snapshots, controls
    stream_client = httpx.AsyncClient()  # broadcaster uses this internally

    app.state.streamers = {
        cam_id: CameraStreamer(cam, stream_client) for cam_id, cam in app.state.cameras.items()
    }
    app.state.sse_subscribers: set[asyncio.Queue] = set()
    app.state.motion_stats = {
        cam_id: {"events_total": 0, "last_at": None} for cam_id in app.state.cameras
    }
    tasks = [
        asyncio.create_task(
            motion_detection_task(
                cam,
                app.state.streamers[cam.id],
                app.state.sse_subscribers,
                app.state.motion_stats,
            ),
            name=f"motion-{cam.id}",
        )
        for cam in app.state.cameras.values()
    ]
    logger.info(
        "OneCam started — %d camera(s): %s",
        len(app.state.cameras),
        ", ".join(app.state.cameras),
    )

    yield

    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    await app.state.api_client.aclose()
    await stream_client.aclose()
    logger.info("OneCam shut down")


app = FastAPI(title="OneCam", version="1.0.0", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(streams_router, prefix="/api")
app.include_router(events_router, prefix="/api")
app.include_router(admin_router, prefix="/api/admin")

_STATIC = Path("static")


@app.get("/ui/", include_in_schema=False)
@app.get("/ui/index.html", include_in_schema=False)
async def serve_ui():
    css_v = int(_STATIC.joinpath("style.css").stat().st_mtime)
    html = _STATIC.joinpath("index.html").read_text()
    return HTMLResponse(html.replace('href="style.css"', f'href="style.css?v={css_v}"'))


@app.get("/admin", include_in_schema=False)
@app.get("/admin/", include_in_schema=False)
async def serve_admin():
    css_v = int(_STATIC.joinpath("style.css").stat().st_mtime)
    html = _STATIC.joinpath("admin.html").read_text()
    return HTMLResponse(html.replace('href="style.css"', f'href="/ui/style.css?v={css_v}"'))


app.mount("/ui", StaticFiles(directory="static"), name="static")


@app.get("/", include_in_schema=False)
async def root():
    return RedirectResponse("/ui/")


@app.get("/health")
async def health(request: Request):
    return {"status": "ok", "cameras": len(request.app.state.cameras)}
