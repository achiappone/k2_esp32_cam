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
// Board: esp32:esp32:esp32cam
// Antenna: the IPEX one that came with it, fitted. These ship with the
// 0 Ohm link on the u.FL connector rather than the trace, so running
// bare costs ~33 dB - see the README.

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

// Tuning knobs, and /set changes all of them live - see below. Focus is NOT
// among them: the OV2640's lens is a threaded barrel you turn by hand, often
// with threadlock on it from the factory. No register fixes a soft image.
// Init at UXGA deliberately: the driver sizes its frame buffers once, at
// init, so starting small and asking for bigger later gets you a buffer that
// cannot hold the frame. Start at the maximum and step DOWN at runtime.
#define FRAME_SIZE FRAMESIZE_UXGA
// ...and what it actually runs at. Measured on this board at quality 10:
//    800x600   11.7 fps   1733 kbit/s
//   1024x768    7.5 fps   1954 kbit/s
//   1280x720    6.5 fps   1935 kbit/s   <- here
//   1280x1024   3.6 fps   1530 kbit/s
//   1600x1200   1.3 fps    874 kbit/s
// Delivered throughput PEAKS at 720p and falls off above it: past that the
// JPEG encoder does more work for less data out, so the bigger sizes cost
// frames and give nothing back. The encoder is the ceiling here, not the
// link - which is why this is a sensor setting and not a WiFi problem.
#define WORKING_FRAME_SIZE FRAMESIZE_HD  // 1280x720
#define JPEG_QUALITY 10  // lower is better quality and a bigger frame

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
  sensor_t *s = esp_camera_sensor_get();
  // Step down from the init size now that the buffers are allocated. Without
  // this it boots at UXGA and serves 1.2 fps until something calls /set.
  s->set_framesize(s, WORKING_FRAME_SIZE);
  // The OV2640 on this module is mounted rotated and mirrored.
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

// Chrome stopped supporting multipart/x-mixed-replace for top-level
// navigations, so browsing straight to /stream paints one frame and then sits
// there looking like a frozen camera. Inside an <img> it still animates, which
// is how the dashboard consumes it anyway - so serve the <img> here and give
// the bare URL something that works.
static const char VIEWER[] =
    "<!doctype html><title>k2 cam</title>"
    "<style>body{margin:0;background:#111;color:#ccc;"
    "font:13px ui-monospace,monospace}"
    "img{width:100vw;height:calc(100vh - 24px);object-fit:contain}"
    "p{margin:0;padding:4px 8px}</style>"
    "<img id=c src=\"/stream\"><p id=h>connecting</p>"
    "<script>let n=0,t=Date.now(),p=null,"
    "v=document.createElement('canvas');v.width=64;v.height=48;"
    "let x=v.getContext('2d',{willReadFrequently:true});"
    "setInterval(()=>{let i=document.getElementById('c');"
    "if(!i.naturalWidth)return;x.drawImage(i,0,0,64,48);"
    "let d=x.getImageData(0,0,64,48).data,f=0;"
    "if(p){for(let k=0;k<d.length;k+=4)f+=Math.abs(d[k]-p[k]);}"
    "if(p&&f>1500)n++;p=d.slice();"
    "document.getElementById('h').textContent="
    "n+' frames  '+(n/((Date.now()-t)/1000)).toFixed(1)+' fps'},100)<\/script>";

// Live sensor tuning: /set?quality=10&ae_level=1&framesize=8 ...
// Exists so that focusing the lens and dialling exposure is a page reload
// rather than a reflash each time. Deliberately a flat allowlist and no
// persistence - whatever you settle on gets written into the defaults above.
static esp_err_t set_handler(httpd_req_t *req) {
  char q[160], v[16];
  sensor_t *s = esp_camera_sensor_get();
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK)
    return httpd_resp_send(req, "usage: /set?quality=10&ae_level=1", HTTPD_RESP_USE_STRLEN);

#define KNOB_T(name, fn, T)                                       \
  if (httpd_query_key_value(q, name, v, sizeof(v)) == ESP_OK) {   \
    s->fn(s, (T)atoi(v));                                         \
  }
#define KNOB(name, fn) KNOB_T(name, fn, int)
  KNOB_T("framesize", set_framesize, framesize_t)  // 8=SVGA 10=SXGA 13=UXGA
  KNOB("quality", set_quality)       // 4..63, lower is better
  KNOB("brightness", set_brightness) // -2..2
  KNOB("contrast", set_contrast)     // -2..2
  KNOB("saturation", set_saturation) // -2..2
  KNOB("ae_level", set_ae_level)     // -2..2, exposure target
  KNOB("aec_value", set_aec_value)   // 0..1200, manual exposure
  KNOB("aec", set_exposure_ctrl)     // 0/1 auto exposure
  KNOB("agc", set_gain_ctrl)         // 0/1 auto gain
  KNOB_T("gainceiling", set_gainceiling, gainceiling_t)
  KNOB("awb", set_whitebal)          // 0/1
  KNOB("vflip", set_vflip)
  KNOB("hmirror", set_hmirror)
#undef KNOB
#undef KNOB_T

  char out[96];
  int n = snprintf(out, sizeof(out), "{\"framesize\":%d,\"quality\":%d,\"ae_level\":%d}",
                   s->status.framesize, s->status.quality, s->status.ae_level);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, n);
}

static esp_err_t viewer_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, VIEWER, HTTPD_RESP_USE_STRLEN);
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

  httpd_uri_t viewer = {"/", HTTP_GET, viewer_handler, NULL};
  httpd_uri_t setq = {"/set", HTTP_GET, set_handler, NULL};
  httpd_uri_t snapshot = {"/snapshot", HTTP_GET, snapshot_handler, NULL};
  httpd_uri_t stream = {"/stream", HTTP_GET, stream_handler, NULL};
  httpd_uri_t healthz = {"/healthz", HTTP_GET, healthz_handler, NULL};

  if (httpd_start(&server, &cfg) == ESP_OK) {
    httpd_register_uri_handler(server, &viewer);
    httpd_register_uri_handler(server, &setq);
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
