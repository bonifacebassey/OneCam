/*
 * OneCam — ESP32-CAM Firmware
 * Board: AI Thinker ESP32-CAM (OV2640)
 *
 * Exposes three HTTP endpoints:
 *   GET /stream   — continuous MJPEG stream (boundary=frame)
 *   GET /capture  — single JPEG snapshot
 *   GET /control?var=<name>&val=<value> — adjust camera parameters
 *
 * Flash instructions:
 *   1. Arduino IDE → Tools → Board → ESP32 Arduino → AI Thinker ESP32-CAM
 *   2. Tools → Port → (your serial adapter port)
 *   3. Bridge GPIO0 to GND before uploading (enters flash mode)
 *   4. Click Upload; after "Connecting…" appears you can release GPIO0
 *   5. After upload: remove the GPIO0 bridge, press RESET button
 *   6. Open Serial Monitor at 115200 baud — the camera IP will be printed
 *   7. Add that IP to cameras.json in the OneCam Python project
 *
 * Tip: reserve a static IP for the camera in your router's DHCP settings
 * using the camera's MAC address so the IP never changes after reboots.
 *
 * Required Arduino library:
 *   esp32 board package by Espressif (v2.x) — includes esp_camera and esp_http_server
 *   Install via: Arduino IDE → Boards Manager → search "esp32" by Espressif Systems
 */

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <WiFi.h>

// ── WiFi credentials ─────────────────────────────────────────────────────────
#define WIFI_SSID "YOUR_SSID_HERE"
#define WIFI_PASS "YOUR_PASSWORD_HERE"

// ── Static IP configuration ───────────────────────────────────────────────────
// Set STATIC_IP to true to use a fixed IP instead of DHCP.
// Make sure the chosen IP is outside your router's DHCP range to avoid conflicts.
#define STATIC_IP       true
IPAddress local_ip(192, 168, 1, 0);   // ← desired camera IP (replace before flashing)
IPAddress gateway(192, 168, 1, 1);    // ← your router IP (replace before flashing)
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

// ── AI Thinker ESP32-CAM GPIO pin map ────────────────────────────────────────
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ── MJPEG stream constants ────────────────────────────────────────────────────
#define PART_BOUNDARY "frame"
static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace; boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ── /stream handler ───────────────────────────────────────────────────────────
static esp_err_t stream_handler(httpd_req_t* req) {
    camera_fb_t* fb = NULL;
    char part_buf[64];

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Frame capture failed");
            res = ESP_FAIL;
            break;
        }

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

    return res;
}

// ── /capture handler ──────────────────────────────────────────────────────────
static esp_err_t capture_handler(httpd_req_t* req) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    return res;
}

// ── /control handler ──────────────────────────────────────────────────────────
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
    int ret = -1;

    if      (!strcmp(var_name, "framesize"))  ret = s->set_framesize(s, (framesize_t)val);
    else if (!strcmp(var_name, "quality"))    ret = s->set_quality(s, val);
    else if (!strcmp(var_name, "brightness")) ret = s->set_brightness(s, val);
    else if (!strcmp(var_name, "contrast"))   ret = s->set_contrast(s, val);
    else if (!strcmp(var_name, "saturation")) ret = s->set_saturation(s, val);
    else if (!strcmp(var_name, "hmirror"))    ret = s->set_hmirror(s, val);
    else if (!strcmp(var_name, "vflip"))      ret = s->set_vflip(s, val);
    else if (!strcmp(var_name, "awb"))        ret = s->set_whitebal(s, val);
    else if (!strcmp(var_name, "agc"))        ret = s->set_gain_ctrl(s, val);
    else if (!strcmp(var_name, "aec"))        ret = s->set_exposure_ctrl(s, val);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (ret < 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ── Start HTTP servers ────────────────────────────────────────────────────────
// Two separate httpd instances so the blocking stream loop (port 81) never
// prevents capture or control requests (port 80) from being served.
static void start_camera_server() {
    httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, NULL };
    httpd_uri_t control_uri = { "/control", HTTP_GET, control_handler, NULL };
    httpd_uri_t stream_uri  = { "/stream",  HTTP_GET, stream_handler,  NULL };

    // API server — port 80
    httpd_config_t api_cfg = HTTPD_DEFAULT_CONFIG();
    api_cfg.server_port      = 80;
    api_cfg.ctrl_port        = 32768;
    api_cfg.max_uri_handlers = 4;

    httpd_handle_t api_server = NULL;
    if (httpd_start(&api_server, &api_cfg) == ESP_OK) {
        httpd_register_uri_handler(api_server, &capture_uri);
        httpd_register_uri_handler(api_server, &control_uri);
        Serial.println("API server started on port 80");
    }

    // Stream server — port 81 (stream handler blocks indefinitely; isolated here)
    httpd_config_t stream_cfg = HTTPD_DEFAULT_CONFIG();
    stream_cfg.server_port      = 81;
    stream_cfg.ctrl_port        = 32769;
    stream_cfg.max_uri_handlers = 2;

    httpd_handle_t stream_server = NULL;
    if (httpd_start(&stream_server, &stream_cfg) == ESP_OK) {
        httpd_register_uri_handler(stream_server, &stream_uri);
        Serial.println("Stream server started on port 81");
    }
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(false);

    camera_config_t config;
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
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_CIF;   // 640×480 — good for LAN streaming
    config.jpeg_quality = 15;              // 4–63, lower = better quality
    config.fb_count     = 2;              // double-buffer prevents tearing
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x — check wiring and reset\n", err);
        return;
    }

    // Sensor tweaks for OV2640
    sensor_t* s = esp_camera_sensor_get();
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_lenc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_wpc(s, 1);

    // Connect to WiFi
    if (STATIC_IP) {
        if (!WiFi.config(local_ip, gateway, subnet, dns)) {
            Serial.println("Static IP configuration failed");
        }
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi failed — check credentials and reset");
        return;
    }

    // Disable modem sleep — keeps the WiFi radio always on, eliminating
    // the 100–800ms wakeup latency spikes that appear even close to the router.
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // max TX power for best signal

    Serial.println("\n=== OneCam ready ===");
    Serial.printf("  Stream:   http://%s:81/stream\n", WiFi.localIP().toString().c_str());
    Serial.printf("  Snapshot: http://%s/capture\n",   WiFi.localIP().toString().c_str());
    Serial.printf("  Control:  http://%s/control?var=framesize&val=8\n",
                  WiFi.localIP().toString().c_str());
    Serial.println("Add this IP to cameras.json in the OneCam Python project.");

    start_camera_server();
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    delay(10000);  // Nothing to do — HTTP server runs in its own FreeRTOS task
}
