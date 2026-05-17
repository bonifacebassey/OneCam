import asyncio
import json

from fastapi import APIRouter, Request
from fastapi.responses import StreamingResponse

router = APIRouter()


@router.get("/events")
async def sse_events(request: Request):
    """Server-Sent Events endpoint for real-time motion alerts.

    Each connected browser client gets its own asyncio.Queue.
    The motion detection task broadcasts events to all subscriber queues.
    """
    local_q: asyncio.Queue = asyncio.Queue(maxsize=100)
    subscribers: set = request.app.state.sse_subscribers
    subscribers.add(local_q)

    async def generator():
        try:
            yield b": connected\n\n"
            while True:
                if await request.is_disconnected():
                    break
                try:
                    event = await asyncio.wait_for(local_q.get(), timeout=15.0)
                    data = json.dumps(event)
                    yield f"event: motion\ndata: {data}\n\n".encode()
                except asyncio.TimeoutError:
                    yield b": keepalive\n\n"
        finally:
            subscribers.discard(local_q)

    return StreamingResponse(
        generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",  # prevents nginx/Cloudflare from buffering SSE
        },
    )
