# OneCam

Home monitoring system built around the **AI Thinker ESP32-CAM**. A lightweight FastAPI server proxies the camera's MJPEG stream and serves a browser dashboard accessible from any device on your network — and optionally from anywhere in the world via Cloudflare Tunnel.

## Features

- **Live MJPEG stream** — watch from any browser on the local network, no plugin needed
- **Multi-camera support** — add as many cameras as you like via `cameras.json`
- **On-demand snapshots** — grab a JPEG of the current frame with one click
- **Camera controls** — adjust resolution, JPEG quality, brightness, and contrast from the browser
- **Motion detection** — background task compares frames; saves a snapshot and sends a real-time browser alert when movement is detected
- **SSE alerts** — browser stays connected via Server-Sent Events; motion badges appear instantly without polling
- **Remote access ready** — one command with [Cloudflare Tunnel](docs/remote-access.md) makes the dashboard reachable from anywhere, for free

## Hardware

| Part | Notes |
|---|---|
| AI Thinker ESP32-CAM | OV2640 camera module, built-in PSRAM |
| USB-to-TTL adapter (FTDI / CH340) | 3.3 V logic, for flashing only |
| Micro-USB or dupont wires | Power + serial connection |

Wiring and flashing instructions: [docs/firmware.md](docs/firmware.md)

## Quick Start

### 1 — Flash the camera

See [docs/firmware.md](docs/firmware.md). After flashing, the Serial Monitor prints:

```
=== OneCam ready ===
  Stream:   http://192.168.1.101/stream
  Snapshot: http://192.168.1.101/capture
```

Note the IP address — you'll need it in the next step.

### 2 — Configure cameras

Edit `cameras.json` and replace the IP with the one printed above:

```json
{
  "cameras": [
    {
      "id": "front-door",
      "label": "Front Door",
      "ip": "192.168.1.101",
      "enabled": true
    }
  ]
}
```

Set a **DHCP reservation** in your router for the camera's MAC address so the IP never changes after a reboot.

### 3 — Install and run

```bash
uv sync

uv run uvicorn main:app --host 0.0.0.0 --reload --port 8000
```

### 4 — Open the dashboard

On the same machine:
```
http://localhost:8000/
```

From any phone, tablet, or laptop on the same WiFi — first find your machine's IP:
```bash
ip route get 8.8.8.8 | awk '{print $7; exit}'
# example output: 192.168.1.50
```

Then open:
```
http://192.168.1.50:8000/
```

## Dashboard

The browser dashboard auto-discovers cameras from `/api/cameras` and displays them in a responsive grid.

| Feature | How to use |
|---|---|
| Live stream | Loads automatically — MJPEG runs natively in any browser |
| Offline indicator | Appears if the camera drops; **Retry** button reconnects |
| Snapshot | Click **Snapshot** to open the current frame in a new tab (also saves to `snapshots/`) |
| Camera controls | Click **Controls** to expand sliders for resolution, quality, brightness, contrast |
| Motion alert | A red badge pulses on the card and a toast notification appears at the bottom-right when motion is detected |

## Adding more cameras

1. Flash another ESP32-CAM with the same sketch (just change the WiFi credentials if needed)
2. Note the new IP from Serial Monitor
3. Add an entry to `cameras.json`:

```json
{
  "cameras": [
    { "id": "front-door", "label": "Front Door",  "ip": "192.168.1.101", "enabled": true },
    { "id": "backyard",   "label": "Backyard",     "ip": "192.168.1.102", "enabled": true },
    { "id": "garage",     "label": "Garage",       "ip": "192.168.1.103", "enabled": false }
  ]
}
```

Cameras with `"enabled": false` are loaded but skipped by the stream proxy and motion detection. Set to `true` to activate.

4. Restart the server.

## Configuration

Copy `.env.example` to `.env` and adjust as needed. All settings have sensible defaults.

| Variable | Default | Description |
|---|---|---|
| `HOST` | `0.0.0.0` | Server bind address |
| `PORT` | `8000` | Server port |
| `CAMERAS_FILE` | `cameras.json` | Path to camera registry |
| `SNAPSHOTS_DIR` | `snapshots` | Directory for saved snapshots |
| `PROXY_TIMEOUT` | `10.0` | Seconds to wait for camera response |
| `MOTION_POLL_INTERVAL` | `2.0` | Seconds between frame grabs for motion detection |
| `MOTION_THRESHOLD` | `0.02` | Mean pixel difference (0–1) that triggers motion |
| `MOTION_COOLDOWN_SECONDS` | `5.0` | Minimum gap between consecutive alerts per camera |
| `DEBUG` | `false` | Enable verbose logging |

## Remote Access

To reach the dashboard from outside your home network — see [docs/remote-access.md](docs/remote-access.md).

Quick test (no account needed):
```bash
cloudflared tunnel --url http://localhost:8000
# Instantly prints a public https://... URL
```

## API Reference

Full endpoint documentation: [docs/api.md](docs/api.md)

## Project Layout

```
OneCam/
├── main.py                  FastAPI app — lifespan, routers, middleware
├── config.py                Settings (pydantic-settings, reads .env)
├── cameras.json             Camera registry — edit to add/remove cameras
│
├── core/
│   ├── camera_registry.py   CameraConfig model, load_cameras(), get_camera()
│   ├── proxy.py             Zero-copy MJPEG byte passthrough via httpx
│   └── motion.py            Background per-camera motion detection task
│
├── api/
│   ├── streams.py           Stream, snapshot, camera list, control endpoints
│   └── events.py            SSE endpoint for real-time motion alerts
│
├── static/
│   ├── index.html           Browser dashboard (vanilla JS)
│   └── style.css            Dark theme
│
├── firmware/
│   └── esp32cam_stream/
│       └── esp32cam_stream.ino   Arduino sketch for AI Thinker ESP32-CAM
│
├── snapshots/               Auto-saved motion snapshots (gitignored)
├── tests/                   pytest test suite
└── docs/                    Extended documentation
```

## Development

```bash
make install   # install dependencies
make run       # start server with live reload
make test      # run tests
make check     # lint + tests (CI gate)
make           # show all available targets
```

Or directly:

```bash
# Run with auto-reload
uv run uvicorn main:app --host 0.0.0.0 --reload --port 8000

# Run tests
uv run pytest tests/ -v

# Interactive API docs (auto-generated by FastAPI)
http://localhost:8000/docs
```
