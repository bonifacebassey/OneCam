import asyncio
import logging
import time
from datetime import datetime
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

from config import settings
from core.broadcaster import CameraStreamer
from core.camera_registry import CameraConfig

logger = logging.getLogger(__name__)


def _to_frame(raw: bytes) -> np.ndarray:
    """Decode JPEG → blurred grayscale float32 array (0–1)."""
    img = Image.open(BytesIO(raw)).convert("L")
    if settings.motion_blur_radius > 0:
        img = img.filter(ImageFilter.GaussianBlur(radius=settings.motion_blur_radius))
    return np.asarray(img, dtype=np.float32) / 255.0


async def motion_detection_task(
    camera: CameraConfig,
    streamer: CameraStreamer,
    sse_subscribers: set[asyncio.Queue],
) -> None:
    """Subscribe to the camera streamer and broadcast an event when real motion is detected.

    Uses a background model (exponential moving average) instead of comparing
    consecutive frames — this makes it immune to gradual lighting changes,
    auto-exposure drift, and JPEG noise.

    A motion event fires only when the fraction of significantly changed pixels
    exceeds motion_threshold, after Gaussian blur to suppress sensor noise.
    """
    snapshots_dir = Path(settings.snapshots_dir)
    background: np.ndarray | None = None
    last_alert_time: float = 0.0

    q = streamer.subscribe(maxsize=2)
    try:
        while True:
            await asyncio.sleep(settings.motion_poll_interval)

            # Drain the queue — we only care about the most recent frame
            raw: bytes | None = None
            try:
                while True:
                    raw = q.get_nowait()
            except asyncio.QueueEmpty:
                pass

            if raw is None:
                continue  # no frame arrived since last check

            try:
                frame = _to_frame(raw)

                if background is None:
                    background = frame.copy()
                else:
                    if frame.shape == background.shape:
                        diff = np.abs(frame - background)

                        # Cancel out global exposure/gain drift (AEC/AGC on the ESP32-CAM).
                        # If the whole frame got uniformly brighter or darker, that shift
                        # shows up equally in every pixel's diff. Subtracting it from the
                        # threshold means only pixels that changed MORE than the global drift
                        # are counted, making the detector blind to lighting adjustments.
                        exposure_drift = abs(float(frame.mean()) - float(background.mean()))
                        effective_threshold = settings.motion_pixel_threshold + exposure_drift

                        changed = float(np.mean(diff > effective_threshold))

                        now = time.monotonic()
                        cooldown_ok = (now - last_alert_time) >= settings.motion_cooldown_seconds

                        if changed > settings.motion_threshold and cooldown_ok:
                            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                            snapshot_name = f"{camera.id}_{ts}.jpg"
                            (snapshots_dir / snapshot_name).write_bytes(raw)
                            last_alert_time = now

                            logger.info(
                                "Motion on %s (%.1f%% pixels changed) → %s",
                                camera.id,
                                changed * 100,
                                snapshot_name,
                            )

                            event = {
                                "camera_id": camera.id,
                                "label": camera.label,
                                "timestamp": ts,
                                "snapshot_file": snapshot_name,
                            }
                            for sub in list(sse_subscribers):
                                sub.put_nowait(event)

                    background = (
                        settings.motion_bg_alpha * frame
                        + (1 - settings.motion_bg_alpha) * background
                    )

            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("Unexpected error in motion task for %s", camera.id)
                background = None  # reset so reconnect doesn't false-positive

    finally:
        streamer.unsubscribe(q)
