# Firmware Guide

Two camera boards are supported:

- [AI Thinker ESP32-CAM](#ai-thinker-esp32-cam) (OV2640) — classic black board, needs an external USB-to-TTL adapter
- [DFRobot DFR1154](#dfrobot-dfr1154) (ESP32-S3 + OV3660 + IR LED + light sensor + PDM mic) — native USB-C, no external adapter

Both sketches expose the same HTTP API (`/stream` on port 81, `/capture` `/control` `/wifi` on port 80, `boundary=frame` MJPEG framing), so you can mix them in one `cameras.json`.

---

## AI Thinker ESP32-CAM

Step-by-step instructions for flashing the OneCam sketch to an AI Thinker ESP32-CAM.

### Hardware Required

- **AI Thinker ESP32-CAM** - the black module with OV2640 camera and antenna connector
- **USB-to-TTL serial adapter** - FTDI FT232RL or CH340G, **must operate at 3.3 V logic**
  (5 V on TX/RX will permanently damage the ESP32)
- Dupont jumper wires (female-female)
- Optional: 10 µF capacitor across the adapter's 3.3 V and GND to stabilise power during flash

### Wiring

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
> Remove the bridge after uploading - leaving it connected prevents the camera from booting.

### Arduino IDE Setup

#### 1 - Install the ESP32 board package

1. Open Arduino IDE
2. `File → Preferences → Additional boards manager URLs`, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. `Tools → Board → Boards Manager` → search **esp32** by Espressif Systems → Install (v2.x)

#### 2 - Select the correct board

`Tools → Board → ESP32 Arduino → AI Thinker ESP32-CAM`

#### 3 - Configure upload settings

| Setting | Value |
|---|---|
| Board | AI Thinker ESP32-CAM |
| Upload Speed | 115200 |
| Flash Mode | DIO |
| Flash Frequency | 80 MHz |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| Port | (your USB-TTL adapter port) |

### Configure the Sketch

Open `firmware/esp32cam_stream/esp32cam_stream.ino` and set your WiFi credentials:

```cpp
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"
```

### Uploading

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

### Verify the camera works

Before touching the Python server, confirm the camera streams correctly from a browser on the same network:

```
http://192.168.1.101/stream
```

You should see the MJPEG stream directly in the browser. If the page loads but the stream is black, try tapping `RESET` again - the OV2640 sometimes needs two power cycles.

### Reserve a Static IP

The camera's IP can change across reboots if your router hands out addresses dynamically. Prevent this by adding a **DHCP reservation** in your router:

1. Find the camera's MAC address - many routers show it in the connected devices list once the camera is online
2. In your router's admin panel, bind that MAC address to a fixed IP (e.g. `192.168.1.101`)
3. This ensures the IP you put in `cameras.json` never changes

### Camera Parameters

The sketch starts with conservative defaults. You can change them at the top of the `setup()` function or via the `/control` endpoint at runtime:

| Parameter | Default | Notes |
|---|---|---|
| `frame_size` | `FRAMESIZE_VGA` | 640×480 - best for LAN streaming |
| `jpeg_quality` | `12` | 4–63, lower = higher quality / larger frames |
| `fb_count` | `2` | Double-buffering prevents frame tearing |

#### Available Frame Sizes

| Name | Resolution | Best for |
|---|---|---|
| `FRAMESIZE_QVGA` | 320×240 | Slow networks, many cameras |
| `FRAMESIZE_VGA` | 640×480 | Default - good balance |
| `FRAMESIZE_SVGA` | 800×600 | More detail, ~30% more bandwidth |
| `FRAMESIZE_XGA` | 1024×768 | High detail, requires good WiFi signal |
| `FRAMESIZE_UXGA` | 1600×1200 | Max resolution - snapshot mode only, too slow for live stream |

### Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `Camera init failed: 0x...` | Bad wiring or power | Check 3.3 V rail; try `CAMERA_FB_IN_DRAM` if no PSRAM detected |
| `WiFi failed` | Wrong credentials | Double-check SSID/password - they are case-sensitive |
| Upload stuck at `Connecting...` | GPIO0 not bridged to GND | Bridge GPIO0 → GND before upload |
| Black stream after boot | OV2640 init issue | Press RESET once more; sometimes needs two cold boots |
| Stream works directly but not via Python server | Content-Type boundary mismatch | Ensure `PART_BOUNDARY` in sketch is `"frame"` (it is by default) |
| Camera IP changes on reboot | No DHCP reservation | Set a static DHCP lease in your router |

---

## DFRobot DFR1154

Step-by-step instructions for flashing the OneCam sketch to a DFRobot DFR1154 ESP32-S3 AI Camera Module ([product page](https://www.dfrobot.com/product-2899.html)).

### Hardware Required

- **DFRobot DFR1154 module** - ESP32-S3R8 (8 MB embedded PSRAM, 16 MB flash) with OV3660 sensor, IR illumination LED, LTR-308 ambient light sensor, PDM microphone, MAX98357 amplifier + speaker connector, microSD slot
- **USB-C cable** - power and flashing, no external adapter needed (the S3 has native USB)

No wiring is required for flashing. The board enters bootloader mode automatically when the IDE invokes `esptool`.

### Arduino IDE Setup

#### 1 - Install the ESP32 board package (v3.x)

The DFR1154 sketch uses the `ledcAttach()` API and `ESP_I2S` library, both of which require the **3.x** Espressif board package. If you only have v2.x installed for the AI Thinker, upgrade — the AI Thinker sketch is compatible with v3.x too.

#### 2 - Install the DFRobot_LTR308 library

`Sketch → Include Library → Manage Libraries` → search **DFRobot_LTR308** → Install. This is the only third-party library the sketch depends on.

#### 3 - Select the correct board

`Tools → Board → ESP32 Arduino → ESP32S3 Dev Module`

#### 4 - Configure upload settings

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** (so `Serial` prints arrive over the USB-C cable) |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | Default 4MB with spiffs |
| PSRAM | **OPI PSRAM** |
| Upload Speed | 921600 |
| Port | (your USB-C port — appears once you plug in) |

### Configure the Sketch

Open `firmware/dfr1154_stream/dfr1154_stream.ino` and set your WiFi credentials and (optional) static IP:

```cpp
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"

#define STATIC_IP true
IPAddress local_ip(192, 168, 1, 102);   // pick something outside your router's DHCP range
IPAddress gateway(192, 168, 1, 1);
```

### Uploading

1. Plug the board into USB-C
2. In Arduino IDE click **Upload**
3. When upload completes the board reboots automatically
4. Open `Tools → Serial Monitor`, baud rate **115200**

The camera prints its status:

```
=== OneCam (DFR1154) ready ===
  Stream:   http://192.168.1.102:81/stream
  Snapshot: http://192.168.1.102/capture
  Light:    http://192.168.1.102/light
  Sound:    http://192.168.1.102/sound
Add this IP to cameras.json in the OneCam Python project.
```

Note: the DFR1154 sketch keeps serial output behind a `#define DEBUG false` guard for performance. Flip to `true` and reflash to see the banner above and reconnect messages.

### Extra Endpoints

Beyond the standard `/stream`, `/capture`, `/control`, `/wifi` shared with the AI Thinker sketch, the DFR1154 firmware exposes:

| Endpoint | Returns |
|---|---|
| `GET /light` | `{"lux": <float>, "raw": <int>, "ir_on": <bool>, "ir_mode": 0\|1\|2}` |
| `GET /sound` | `{"rms": <int>, "peak": <int>, "samples": <int>, "rate": 16000}` — captures ~100 ms of PDM mic samples; don't poll faster than ~5 Hz |

It also extends `/control` with two extra variables:

| Variable | Range | Effect |
|---|---|---|
| `ir` | `0`, `1`, `2` | IR LED off / forced on / auto (driven by light sensor with hysteresis) |
| `ir_brightness` | `0–255` | PWM duty for the IR LED when forced on or auto-on |

In **auto** mode (the default), the LTR-308 reading drives the IR LED: it turns on below ~5 lux and back off above ~15 lux. Adjust `LUX_DARK_THRESHOLD` and `LUX_LIGHT_THRESHOLD` near the top of the sketch to retune.

### Board GPIO Reference

For convenience when wiring up extras (speaker, SD card, Gravity sensors):

| Peripheral | Pin(s) |
|---|---|
| Camera SCCB (I²C, shared with light sensor) | SDA=8, SCL=9 |
| Camera data | D0=16, D1=18, D2=21, D3=17, D4=14, D5=7, D6=6, D7=4 |
| Camera clocks | XCLK=5, PCLK=15, VSYNC=1, HREF=2 |
| IR illumination LED | GPIO47 (PWM via LEDC) |
| Status LED | GPIO3 |
| PDM microphone | CLK=GPIO38, DATA=GPIO39 |
| MAX98357 speaker amp (I²S) | BCLK=45, LRC=46, DOUT=42 |
| microSD (SPI) | CS=10, MOSI=11, MISO=13, SCK=12 |

### Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `ledcAttach` undefined | esp32 board package v2.x installed | Upgrade to v3.x in Boards Manager |
| `DFRobot_LTR308.h: No such file` | Library not installed | Install **DFRobot_LTR308** via Library Manager |
| `/light` returns `{"error":"ltr308 not ready"}` | LTR-308 init ran before camera init | Should not happen with this sketch — file an issue |
| No serial output after upload | `USB CDC On Boot` disabled | Enable it in Tools menu and reflash |
| Upload fails repeatedly | Wrong port or board picked | Confirm **ESP32S3 Dev Module** is selected and the USB-C port matches |
| IR LED never turns on at night | Light sensor reading too high | Cover the sensor with your hand; if `/light` lux drops below 5 the LED should kick in. Otherwise check `LUX_DARK_THRESHOLD` |
