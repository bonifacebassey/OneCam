/*
 * OneCam — DFR1154 Firmware
 * Board: DFRobot ESP32-S3 AI Camera Module (SKU DFR1154)
 *   - ESP32-S3R8 (8MB embedded PSRAM), 16MB flash, native USB-C
 *   - OV3660 sensor, 2MP, 160° FOV, 940nm IR-sensitive
 *   - IR illumination LED on GPIO47
 *   - Status LED on GPIO3
 *   - LTR-308 ambient light sensor on the camera's SCCB I2C bus
 *   - PDM microphone (CLK=GPIO38, DATA=GPIO39)
 *
 * HTTP endpoints (the Python server treats this board identically to the AI Thinker
 * sketch — same /stream, /capture, /control, /wifi paths and port split):
 *   GET /stream                            — MJPEG (port 81, boundary=frame)
 *   GET /capture                           — single JPEG (port 80)
 *   GET /control?var=<name>&val=<int>      — sensor + board controls (port 80)
 *   GET /wifi                              — {"rssi": <dBm>}
 *   GET /light                             — {"lux": <float>, "raw": <int>}  (LTR-308)
 *   GET /sound                             — {"rms": <int>, "peak": <int>, "rate": 16000}
 *
 * /control variables (in addition to the OV3660 sensor controls):
 *   var=ir,            val=0|1|2          — IR LED off / on / auto-from-light-sensor
 *   var=ir_brightness, val=0..255         — PWM duty for the IR LED when forced on
 *
 * Required Arduino libraries:
 *   - esp32 board package by Espressif, v3.x  (provides ESP_I2S + new ledcAttach API)
 *   - DFRobot_LTR308                          (install from Library Manager)
 *
 * Arduino IDE settings:
 *   Board:           "ESP32S3 Dev Module"
 *   USB CDC On Boot: Enabled
 *   Flash Size:      16MB
 *   Partition:       "Default 4MB with spiffs" (or larger if doing OTA)
 *   PSRAM:           "OPI PSRAM"
 *   Upload:          plug in the Type-C cable; no BOOT button trick is needed.
 */

#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include <Wire.h>
#include <DFRobot_LTR308.h>
#include "ESP_I2S.h"
#include <math.h>

// ── Debug logging ─────────────────────────────────────────────────────────────
#define DEBUG false
#if DEBUG
  #define LOG(msg)  Serial.println(msg)
  #define LOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(msg)
  #define LOGF(...)
#endif

// ── WiFi credentials ─────────────────────────────────────────────────────────
#define WIFI_SSID "YOUR_SSID_HERE"
#define WIFI_PASS "YOUR_PASSWORD_HERE"

// ── Static IP configuration ──────────────────────────────────────────────────
#define STATIC_IP       true
IPAddress local_ip(192, 168, 1, 0);   // ← desired camera IP
IPAddress gateway(192, 168, 1, 1);    // ← your router IP
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

// ── DFR1154 GPIO map ─────────────────────────────────────────────────────────
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      5
#define SIOD_GPIO_NUM      8
#define SIOC_GPIO_NUM      9
#define Y9_GPIO_NUM        4
#define Y8_GPIO_NUM        6
#define Y7_GPIO_NUM        7
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       17
#define Y4_GPIO_NUM       21
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       16
#define VSYNC_GPIO_NUM     1
#define HREF_GPIO_NUM      2
#define PCLK_GPIO_NUM     15

#define IR_LED_PIN        47
#define STATUS_LED_PIN     3
#define PDM_CLK_PIN       38
#define PDM_DATA_PIN      39

// PWM channel for the IR LED. Camera XCLK already uses LEDC channel 0.
#define IR_LEDC_FREQ      5000
#define IR_LEDC_RES_BITS     8   // duty range 0..255

// Auto-IR thresholds (lux). Hysteresis avoids flicker around dawn/dusk.
#define LUX_DARK_THRESHOLD   5.0f
#define LUX_LIGHT_THRESHOLD 15.0f
#define LIGHT_POLL_MS       3000

// ── MJPEG framing ────────────────────────────────────────────────────────────
#define PART_BOUNDARY "frame"
static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace; boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ── Module state ─────────────────────────────────────────────────────────────
enum IrMode { IR_OFF = 0, IR_ON = 1, IR_AUTO = 2 };
static volatile IrMode  ir_mode       = IR_AUTO;
static volatile uint8_t ir_brightness = 200;   // duty when forced on / auto-on
static volatile bool    ir_currently_on = false;
static volatile uint32_t active_streams = 0;

static DFRobot_LTR308 light_sensor;
static bool           light_ready = false;
static volatile float last_lux    = -1.0f;
static volatile uint32_t last_lux_raw = 0;

static I2SClass mic;
static bool     mic_ready = false;

// ── IR LED control ───────────────────────────────────────────────────────────
static void irApplyHardware(bool on) {
    ledcWrite(IR_LED_PIN, on ? ir_brightness : 0);
    ir_currently_on = on;
}

static void irSetMode(IrMode mode) {
    ir_mode = mode;
    if (mode == IR_ON)       irApplyHardware(true);
    else if (mode == IR_OFF) irApplyHardware(false);
    // AUTO: handled by light-sensor poll loop
}

// ── /stream ──────────────────────────────────────────────────────────────────
static esp_err_t stream_handler(httpd_req_t* req) {
    camera_fb_t* fb = NULL;
    char part_buf[64];

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    active_streams++;
    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) { LOG("Frame capture failed"); res = ESP_FAIL; break; }

        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);

        esp_camera_fb_return(fb);
        fb = NULL;

        if (res != ESP_OK) break;  // client disconnected
    }
    if (active_streams) active_streams--;
    return res;
}

// ── /capture ─────────────────────────────────────────────────────────────────
static esp_err_t capture_handler(httpd_req_t* req) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    return res;
}

// ── /control ─────────────────────────────────────────────────────────────────
static esp_err_t control_handler(httpd_req_t* req) {
    char query[256];
    char var_name[64];
    char val_str[16];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_query_key_value(query, "var", var_name, sizeof(var_name));
    httpd_query_key_value(query, "val", val_str, sizeof(val_str));
    int val = atoi(val_str);

    sensor_t* s = esp_camera_sensor_get();
    int ret = 0;

    if      (!strcmp(var_name, "framesize"))     ret = s->set_framesize(s, (framesize_t)val);
    else if (!strcmp(var_name, "quality"))       ret = s->set_quality(s, val);
    else if (!strcmp(var_name, "brightness"))    ret = s->set_brightness(s, val);
    else if (!strcmp(var_name, "contrast"))      ret = s->set_contrast(s, val);
    else if (!strcmp(var_name, "saturation"))    ret = s->set_saturation(s, val);
    else if (!strcmp(var_name, "hmirror"))       ret = s->set_hmirror(s, val);
    else if (!strcmp(var_name, "vflip"))         ret = s->set_vflip(s, val);
    else if (!strcmp(var_name, "awb"))           ret = s->set_whitebal(s, val);
    else if (!strcmp(var_name, "agc"))           ret = s->set_gain_ctrl(s, val);
    else if (!strcmp(var_name, "aec"))           ret = s->set_exposure_ctrl(s, val);
    else if (!strcmp(var_name, "ir")) {
        if (val < 0 || val > 2) ret = -1;
        else irSetMode((IrMode)val);
    }
    else if (!strcmp(var_name, "ir_brightness")) {
        if (val < 0 || val > 255) ret = -1;
        else {
            ir_brightness = (uint8_t)val;
            if (ir_currently_on) irApplyHardware(true);  // re-apply new duty
        }
    }
    else ret = -1;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (ret < 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ── /wifi ────────────────────────────────────────────────────────────────────
static esp_err_t wifi_handler(httpd_req_t* req) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "{\"rssi\":%d}", WiFi.RSSI());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

// ── /light ───────────────────────────────────────────────────────────────────
static esp_err_t light_handler(httpd_req_t* req) {
    char buf[96];
    int len;
    if (!light_ready) {
        len = snprintf(buf, sizeof(buf), "{\"error\":\"ltr308 not ready\"}");
    } else {
        len = snprintf(buf, sizeof(buf),
                       "{\"lux\":%.2f,\"raw\":%lu,\"ir_on\":%s,\"ir_mode\":%d}",
                       last_lux, (unsigned long)last_lux_raw,
                       ir_currently_on ? "true" : "false", (int)ir_mode);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

// ── /sound ───────────────────────────────────────────────────────────────────
// Reads ~100ms of PDM mic samples and returns RMS + peak. Cheap enough to
// poll once a second from the Python motion task for a sound-alert channel.
static esp_err_t sound_handler(httpd_req_t* req) {
    char buf[96];
    int len;

    if (!mic_ready) {
        len = snprintf(buf, sizeof(buf), "{\"error\":\"mic not ready\"}");
    } else {
        const size_t SAMPLE_COUNT = 1600;            // 100ms @ 16kHz
        int16_t samples[SAMPLE_COUNT];
        size_t bytes = mic.readBytes((char*)samples, sizeof(samples));
        size_t n = bytes / sizeof(int16_t);

        uint64_t sumsq = 0;
        uint16_t peak  = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t s = samples[i];
            sumsq += (uint64_t)(s * s);
            uint16_t a = s < 0 ? (uint16_t)(-s) : (uint16_t)s;
            if (a > peak) peak = a;
        }
        uint32_t rms = n ? (uint32_t)sqrt((double)sumsq / (double)n) : 0;

        len = snprintf(buf, sizeof(buf),
                       "{\"rms\":%lu,\"peak\":%u,\"samples\":%u,\"rate\":16000}",
                       (unsigned long)rms, peak, (unsigned)n);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

// ── HTTP server bring-up ─────────────────────────────────────────────────────
// Two httpd instances: stream blocks indefinitely on port 81 so it can never
// stall capture/control on port 80.
static void start_camera_server() {
    httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, NULL };
    httpd_uri_t control_uri = { "/control", HTTP_GET, control_handler, NULL };
    httpd_uri_t wifi_uri    = { "/wifi",    HTTP_GET, wifi_handler,    NULL };
    httpd_uri_t light_uri   = { "/light",   HTTP_GET, light_handler,   NULL };
    httpd_uri_t sound_uri   = { "/sound",   HTTP_GET, sound_handler,   NULL };
    httpd_uri_t stream_uri  = { "/stream",  HTTP_GET, stream_handler,  NULL };

    httpd_config_t api_cfg = HTTPD_DEFAULT_CONFIG();
    api_cfg.server_port      = 80;
    api_cfg.ctrl_port        = 32768;
    api_cfg.max_uri_handlers = 8;

    httpd_handle_t api_server = NULL;
    if (httpd_start(&api_server, &api_cfg) == ESP_OK) {
        httpd_register_uri_handler(api_server, &capture_uri);
        httpd_register_uri_handler(api_server, &control_uri);
        httpd_register_uri_handler(api_server, &wifi_uri);
        httpd_register_uri_handler(api_server, &light_uri);
        httpd_register_uri_handler(api_server, &sound_uri);
        LOG("API server started on port 80");
    }

    httpd_config_t stream_cfg = HTTPD_DEFAULT_CONFIG();
    stream_cfg.server_port      = 81;
    stream_cfg.ctrl_port        = 32769;
    stream_cfg.max_uri_handlers = 2;

    httpd_handle_t stream_server = NULL;
    if (httpd_start(&stream_server, &stream_cfg) == ESP_OK) {
        httpd_register_uri_handler(stream_server, &stream_uri);
        LOG("Stream server started on port 81");
    }
}

// ── Camera init ──────────────────────────────────────────────────────────────
static bool init_camera() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;    // 640x480 — same default as the other sketch
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        LOGF("Camera init failed: 0x%x\n", err);
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_lenc(s, 1);
    return true;
}

// ── Peripheral init ──────────────────────────────────────────────────────────
static void init_status_led() {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
}

static void init_ir_led() {
    // Arduino-ESP32 v3.x: ledcAttach picks a free channel automatically.
    ledcAttach(IR_LED_PIN, IR_LEDC_FREQ, IR_LEDC_RES_BITS);
    ledcWrite(IR_LED_PIN, 0);
}

static void init_light_sensor() {
    // LTR-308 sits on the camera's SCCB I2C bus — must be brought up AFTER
    // esp_camera_init claims those pins, otherwise reads return 0.
    if (light_sensor.begin()) {
        light_ready = true;
        LOG("LTR-308 ready");
    } else {
        LOG("LTR-308 init failed");
    }
}

static void init_mic() {
    mic.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);
    if (mic.begin(I2S_MODE_PDM_RX, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        mic_ready = true;
        LOG("PDM mic ready");
    } else {
        LOG("PDM mic init failed");
    }
}

// ── Background ticks (called from loop) ──────────────────────────────────────
static void tick_light_and_ir(uint32_t now_ms) {
    static uint32_t last = 0;
    if (!light_ready) return;
    if (now_ms - last < LIGHT_POLL_MS) return;
    last = now_ms;

    uint32_t raw = light_sensor.getData();
    float lux    = light_sensor.getLux(raw);
    last_lux_raw = raw;
    last_lux     = lux;

    if (ir_mode != IR_AUTO) return;
    if (!ir_currently_on && lux <= LUX_DARK_THRESHOLD)  irApplyHardware(true);
    else if (ir_currently_on && lux >= LUX_LIGHT_THRESHOLD) irApplyHardware(false);
}

static void tick_status_led(uint32_t now_ms) {
    static uint32_t last = 0;
    static bool on = false;
    // Slow heartbeat when idle (1Hz), fast when a stream is active (4Hz).
    uint32_t period = active_streams ? 125 : 500;
    if (now_ms - last < period) return;
    last = now_ms;
    on = !on;
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

static void tick_wifi(uint32_t now_ms) {
    static uint32_t last = 0;
    if (now_ms - last < 10000) return;
    last = now_ms;
    if (WiFi.status() == WL_CONNECTED) return;

    LOG("WiFi lost — reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long deadline = millis() + 30000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(500);

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        LOGF("Reconnected — IP: %s\n", WiFi.localIP().toString().c_str());
    }
}

// ── setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(false);

    init_status_led();
    init_ir_led();

    if (!init_camera()) {
        LOG("Camera init failed — halting.");
        return;
    }
    init_light_sensor();  // must be AFTER camera init
    init_mic();

    if (STATIC_IP) {
        if (!WiFi.config(local_ip, gateway, subnet, dns)) LOG("Static IP config failed");
    }
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    LOGF("Connecting to WiFi");
    unsigned long deadline = millis() + 30000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(500);
        LOGF(".");
    }
    LOG("");
    if (WiFi.status() != WL_CONNECTED) {
        LOG("WiFi failed on boot — rebooting");
        ESP.restart();
    }

    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    LOG("\n=== OneCam (DFR1154) ready ===");
    LOGF("  Stream:   http://%s:81/stream\n", WiFi.localIP().toString().c_str());
    LOGF("  Snapshot: http://%s/capture\n",   WiFi.localIP().toString().c_str());
    LOGF("  Light:    http://%s/light\n",     WiFi.localIP().toString().c_str());
    LOGF("  Sound:    http://%s/sound\n",     WiFi.localIP().toString().c_str());
    LOG("Add this IP to cameras.json in the OneCam Python project.");

    start_camera_server();
}

// ── loop ─────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    tick_light_and_ir(now);
    tick_status_led(now);
    tick_wifi(now);
    delay(50);
}
