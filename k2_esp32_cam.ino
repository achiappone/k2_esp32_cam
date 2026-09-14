// Secondary camera for the K2 dashboard: AI-Thinker ESP32-CAM, MJPEG over
// plain HTTP on port 80.
//
// Deliberately not the stock CameraWebServer example. That is four files, a
// ~100 KB gzipped settings UI nobody will open (the dashboard proxies this,
// it is not browsed directly), and it puts the stream on :81. The endpoints
// below match the contract camrelay.py already serves in pve-stack -
// /snapshot, /stream, /healthz - so the dashboard can treat the printer's
// WebRTC feed and this one identically.
//
// Board: esp32:esp32:esp32cam    Antenna: PCB trace (u.FL jumper untouched)

#include <WiFi.h>
#include <WiFiMulti.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "wifi_creds.h"

// AI-Thinker pin map, inlined rather than dragging in the example's
// camera_pins.h - this board is the only one this sketch runs on.
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#define FLASH_GPIO_NUM 4  // the blinding white LED, held off

// Tuning knobs. SVGA at quality 12 is ~25 KB a frame, which a 2.4 GHz link
// carries at well over the 4 fps the dashboard asks for. Raise FRAME_SIZE
// once you have seen the RSSI in its final mounting spot, not before.
#define FRAME_SIZE FRAMESIZE_SVGA
#define JPEG_QUALITY 12  // lower is better quality and a bigger frame

static const char *BOUNDARY = "frameboundary";
static httpd_handle_t server = NULL;

static bool camera_start() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAME_SIZE;
  c.jpeg_quality = JPEG_QUALITY;
  // Two buffers and GRAB_LATEST: a slow reader gets the newest frame rather
  // than a queue of stale ones. Without PSRAM there is only room for one.
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.fb_count = psramFound() ? 2 : 1;
  c.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("camera init failed: 0x%x\n", err);
    return false;
  }
  // The OV2640 on this module is mounted rotated and mirrored.
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
  return true;
}

static esp_err_t snapshot_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  char part[80];
  esp_err_t res = httpd_resp_set_type(
      req, "multipart/x-mixed-replace;boundary=" "frameboundary");
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  // Runs until the client goes away: send_chunk returns non-OK on a closed
  // socket, which is the only exit. One viewer at a time by design - the
  // tunnel is the only client.
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }
    int n = snprintf(part, sizeof(part),
                     "\r\n--%s\r\nContent-Type: image/jpeg\r\n"
                     "Content-Length: %u\r\n\r\n",
                     BOUNDARY, fb->len);
    res = httpd_resp_send_chunk(req, part, n);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
  }
  return res;
}

static esp_err_t healthz_handler(httpd_req_t *req) {
  char body[192];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                   "\"psram\":%s,\"heap\":%u,\"uptime_s\":%lu}",
                   WiFi.status() == WL_CONNECTED ? "true" : "false",
                   WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                   WiFi.RSSI(), psramFound() ? "true" : "false",
                   (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, n);
}

static void server_start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.stack_size = 8192;  // the default 4 K is tight for the stream handler
  cfg.max_open_sockets = 3;
  cfg.lru_purge_enable = true;

  httpd_uri_t snapshot = {"/snapshot", HTTP_GET, snapshot_handler, NULL};
  httpd_uri_t stream = {"/stream", HTTP_GET, stream_handler, NULL};
  httpd_uri_t healthz = {"/healthz", HTTP_GET, healthz_handler, NULL};

  if (httpd_start(&server, &cfg) == ESP_OK) {
    httpd_register_uri_handler(server, &snapshot);
    httpd_register_uri_handler(server, &stream);
    httpd_register_uri_handler(server, &healthz);
  }
}

WiFiMulti wifiMulti;

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);

  if (!camera_start()) {
    // A camera that will not init is almost always the ribbon seated badly or
    // a 5 V rail that sags at init. Say so and reboot rather than serve black.
    Serial.println("no camera - check the ribbon and the supply; rebooting");
    delay(5000);
    ESP.restart();
  }
  Serial.printf("psram: %s\n", psramFound() ? "yes" : "no");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep adds seconds of latency to the stream
  wifiMulti.addAP(HOME_SSID, HOME_PW);
  wifiMulti.addAP(WORK_SSID, WORK_PW);
  Serial.print("wifi");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\n%s  %s  %d dBm\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  server_start();
  Serial.printf("http://%s/stream\n", WiFi.localIP().toString().c_str());
}

void loop() {
  // No-op while connected; re-picks an AP after a drop, which is the whole
  // reason both networks are registered.
  wifiMulti.run();
  delay(1000);
}
