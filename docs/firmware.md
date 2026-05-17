# Firmware Guide

Step-by-step instructions for flashing the OneCam sketch to an AI Thinker ESP32-CAM.

## Hardware Required

- **AI Thinker ESP32-CAM** — the black module with OV2640 camera and antenna connector
- **USB-to-TTL serial adapter** — FTDI FT232RL or CH340G, **must operate at 3.3 V logic**
  (5 V on TX/RX will permanently damage the ESP32)
- Dupont jumper wires (female-female)
- Optional: 10 µF capacitor across the adapter's 3.3 V and GND to stabilise power during flash

## Wiring

Connect the USB-to-TTL adapter to the ESP32-CAM as follows:

```
USB-TTL adapter          ESP32-CAM
─────────────────────    ──────────────────────────
GND              ───────  GND
3.3V             ───────  3.3V (or 5V → VCC if adapter has 5V rail)
TX               ───────  U0R  (UART0 receive)
RX               ───────  U0T  (UART0 transmit)
GND              ───────  GPIO0   ← bridge this to GND only during flashing
```

> **Important:** GPIO0 must be pulled LOW (connected to GND) to enter flash mode.
> Remove the bridge after uploading — leaving it connected prevents the camera from booting.

## Arduino IDE Setup

### 1 — Install the ESP32 board package

1. Open Arduino IDE
2. `File → Preferences → Additional boards manager URLs`, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. `Tools → Board → Boards Manager` → search **esp32** by Espressif Systems → Install (v2.x)

### 2 — Select the correct board

`Tools → Board → ESP32 Arduino → AI Thinker ESP32-CAM`

### 3 — Configure upload settings

| Setting | Value |
|---|---|
| Board | AI Thinker ESP32-CAM |
| Upload Speed | 115200 |
| Flash Mode | DIO |
| Flash Frequency | 80 MHz |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| Port | (your USB-TTL adapter port) |

## Configure the Sketch

Open `firmware/esp32cam_stream/esp32cam_stream.ino` and set your WiFi credentials:

```cpp
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"
```

## Uploading

1. Bridge **GPIO0 → GND** (enter flash mode)
2. Plug in the USB-TTL adapter
3. In Arduino IDE click **Upload**
4. Wait for `Connecting...` to appear in the console, then hold the **RESET** button briefly if needed
5. Once upload completes: **remove the GPIO0-GND bridge**
6. Press **RESET**
7. Open `Tools → Serial Monitor`, set baud rate to **115200**

The camera prints its status:

```
=== OneCam ready ===
  Stream:   http://192.168.1.101/stream
  Snapshot: http://192.168.1.101/capture
  Control:  http://192.168.1.101/control?var=framesize&val=8
Add this IP to cameras.json in the OneCam Python project.
```

## Verify the camera works

Before touching the Python server, confirm the camera streams correctly from a browser on the same network:

```
http://192.168.1.101/stream
```

You should see the MJPEG stream directly in the browser. If the page loads but the stream is black, try tapping `RESET` again — the OV2640 sometimes needs two power cycles.

## Reserve a Static IP

The camera's IP can change across reboots if your router hands out addresses dynamically. Prevent this by adding a **DHCP reservation** in your router:

1. Find the camera's MAC address — many routers show it in the connected devices list once the camera is online
2. In your router's admin panel, bind that MAC address to a fixed IP (e.g. `192.168.1.101`)
3. This ensures the IP you put in `cameras.json` never changes

## Camera Parameters

The sketch starts with conservative defaults. You can change them at the top of the `setup()` function or via the `/control` endpoint at runtime:

| Parameter | Default | Notes |
|---|---|---|
| `frame_size` | `FRAMESIZE_VGA` | 640×480 — best for LAN streaming |
| `jpeg_quality` | `12` | 4–63, lower = higher quality / larger frames |
| `fb_count` | `2` | Double-buffering prevents frame tearing |

### Available Frame Sizes

| Name | Resolution | Best for |
|---|---|---|
| `FRAMESIZE_QVGA` | 320×240 | Slow networks, many cameras |
| `FRAMESIZE_VGA` | 640×480 | Default — good balance |
| `FRAMESIZE_SVGA` | 800×600 | More detail, ~30% more bandwidth |
| `FRAMESIZE_XGA` | 1024×768 | High detail, requires good WiFi signal |
| `FRAMESIZE_UXGA` | 1600×1200 | Max resolution — snapshot mode only, too slow for live stream |

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `Camera init failed: 0x...` | Bad wiring or power | Check 3.3 V rail; try `CAMERA_FB_IN_DRAM` if no PSRAM detected |
| `WiFi failed` | Wrong credentials | Double-check SSID/password — they are case-sensitive |
| Upload stuck at `Connecting...` | GPIO0 not bridged to GND | Bridge GPIO0 → GND before upload |
| Black stream after boot | OV2640 init issue | Press RESET once more; sometimes needs two cold boots |
| Stream works directly but not via Python server | Content-Type boundary mismatch | Ensure `PART_BOUNDARY` in sketch is `"frame"` (it is by default) |
| Camera IP changes on reboot | No DHCP reservation | Set a static DHCP lease in your router |
