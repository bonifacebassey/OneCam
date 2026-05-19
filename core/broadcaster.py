import asyncio
import logging

import httpx

from config import settings
from core.camera_registry import CameraConfig

logger = logging.getLogger(__name__)

_BOUNDARY = b"--frame"
_HEADER_SEP = b"\r\n\r\n"


class CameraStreamer:
    """Maintains a single HTTP connection to one ESP32 camera and fans out
    raw JPEG frames to every subscriber queue.

    Connects only while at least one subscriber exists. When the last
    subscriber leaves the connection is closed, relieving the ESP32.
    Slow subscribers drop frames rather than blocking the broadcaster.
    """

    def __init__(self, camera: CameraConfig, http_client: httpx.AsyncClient):
        self.camera = camera
        self._client = http_client
        self._subscribers: set[asyncio.Queue] = set()
        self._task: asyncio.Task | None = None
        self._buf = bytearray()

    # ── Subscription ───────────────────────────────────────────────────────────

    def subscribe(self, maxsize: int = 5) -> asyncio.Queue:
        """Return a queue that will receive raw JPEG bytes per frame."""
        q: asyncio.Queue = asyncio.Queue(maxsize=maxsize)
        self._subscribers.add(q)
        if self._task is None or self._task.done():
            self._task = asyncio.create_task(self._run(), name=f"streamer-{self.camera.id}")
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self._subscribers.discard(q)
        if not self._subscribers and self._task and not self._task.done():
            self._task.cancel()
            self._task = None

    # ── Streaming loop ─────────────────────────────────────────────────────────

    async def _run(self) -> None:
        delay = 2.0
        while self._subscribers:
            try:
                await self._connect_and_stream()
                delay = 2.0  # connection worked — reset backoff for next disconnect
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                logger.warning(
                    "Stream lost for %s — %s: %s — retrying in %.0fs",
                    self.camera.id,
                    type(exc).__name__,
                    exc,
                    delay,
                )
            if self._subscribers:
                await asyncio.sleep(delay)
                delay = min(delay * 2, 15.0)

    async def _connect_and_stream(self) -> None:
        self._buf.clear()
        # connect timeout keeps the retry loop responsive if the camera is down.
        # read timeout of 10s detects abrupt disconnects (WiFi dropout, power loss) where
        # TCP stays "open" but no data arrives — the ESP32 streams multiple frames per
        # second so 10s of silence means the camera is genuinely gone.
        timeout = httpx.Timeout(connect=settings.proxy_timeout, read=10.0, write=None, pool=None)
        async with self._client.stream("GET", self.camera.stream_url, timeout=timeout) as resp:
            async for chunk in resp.aiter_bytes(chunk_size=settings.stream_chunk_size):
                self._buf += chunk
                self._parse_and_broadcast()

    def _parse_and_broadcast(self) -> None:
        """Extract complete JPEG frames from the raw MJPEG buffer and broadcast them."""
        buf = self._buf
        while True:
            start = buf.find(_BOUNDARY)
            if start == -1:
                break

            header_end = buf.find(_HEADER_SEP, start + len(_BOUNDARY))
            if header_end == -1:
                break  # headers not fully received yet

            jpeg_start = header_end + len(_HEADER_SEP)
            next_start = buf.find(_BOUNDARY, jpeg_start)
            if next_start == -1:
                break  # JPEG not fully received yet

            jpeg_end = next_start
            while jpeg_end > jpeg_start and buf[jpeg_end - 1] in (13, 10):  # \r, \n
                jpeg_end -= 1

            jpeg = bytes(buf[jpeg_start:jpeg_end])
            if jpeg:
                self._push(jpeg)

            del buf[:next_start]  # in-place trim — no reallocation

        if len(buf) > 200_000:  # drop corrupted / oversized buffer
            buf.clear()

    def _push(self, jpeg: bytes) -> None:
        for q in list(self._subscribers):
            if q.full():
                try:
                    q.get_nowait()  # evict oldest frame so newest always wins
                except asyncio.QueueEmpty:
                    pass
            try:
                q.put_nowait(jpeg)
            except asyncio.QueueFull:
                pass


def mjpeg_frame(jpeg: bytes) -> bytes:
    """Wrap raw JPEG bytes in a proper MJPEG multipart frame."""
    return (
        b"\r\n--frame\r\n"
        b"Content-Type: image/jpeg\r\n"
        b"Content-Length: " + str(len(jpeg)).encode() + b"\r\n"
        b"\r\n" + jpeg
    )
