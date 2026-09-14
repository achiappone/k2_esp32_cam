# k2_esp32_cam

Firmware for the secondary camera on the K2 Plus dashboard. AI-Thinker
ESP32-CAM, MJPEG over plain HTTP on port 80.

The dashboard itself lives in `achiappone/k2plus-dashboard`, and the host it
runs on is `achiappone/pve-stack`.

## Endpoints

    /           viewer page - the stream in an <img>, with a live fps readout
    /snapshot   one JPEG
    /stream     multipart/x-mixed-replace MJPEG
    /healthz    json - ssid, ip, rssi, psram, heap, uptime
    /set        live sensor tuning, e.g. /set?framesize=13&quality=10&ae_level=1

These deliberately match what `camera/camrelay.py` in pve-stack already serves
for the printer's own camera, so the dashboard can proxy either one the same
way and does not need to know which kind of camera is behind a tile.

## Why it is shaped this way

**Not the stock `CameraWebServer` example.** That is four files and a ~100 KB
gzipped settings UI, and it puts the stream on `:81`. Nobody will ever open
that UI - the dashboard proxies this feed rather than browsing to it - and
moving the stream to `:80` by editing `app_httpd.cpp` was a bigger diff than
writing the handler. One file, no blob, one port.

**Nothing like `camrelay.py` is needed here.** The printer's camera is
WebRTC-only with a DTLS stack that will not talk to anything but a real
browser, which is why pve-stack runs a headless Chromium beside it and lifts
frames off a `<video>` element. That relay costs ~870 MB and took the host to
98 C before it was made demand-driven. This board emits JPEG frames directly;
a tunnel forwards them without complaint and the host does no work at all.

**Both WiFi networks are compiled in.** `WiFiMulti` registers home and shop and
joins whichever is in range, so the same firmware boots at either site with no
reflash. `run()` is called from `loop()` as well, which is what re-picks an AP
after a drop.

**The address must be a DHCP reservation, not mDNS.** pve-stack commit 25bb2fa
is an afternoon lost to exactly this: the printer's network stack could not
resolve a `.local` name, so a session that looked connected from our end was
half-open and silently dead. Give this board a reserved lease and point the
dashboard at the number.

**It serves its own viewer page.** Chrome no longer supports
`multipart/x-mixed-replace` for top-level navigations: browsing straight to
`/stream` paints the first frame and then sits there, looking exactly like a
frozen camera while `curl` measures a perfectly healthy 5 fps. It still works
inside an `<img>`, which is how the dashboard consumes it - so `/` serves that
`<img>` and the bare URL does the obvious thing.

## Hard-won details

* **`WiFi.setSleep(false)`.** Modem sleep is on by default and adds seconds of
  latency to the stream in a way that looks like a bad camera.
* **`CAMERA_GRAB_LATEST` with `fb_count = 2`.** A slow reader gets the newest
  frame instead of draining a queue of stale ones. Falls back to one buffer
  when PSRAM is absent, which `/healthz` reports so you can tell.
* **GPIO 4 is the blinding white flash LED** and is driven low at boot. It will
  come on by itself otherwise.
* **The IPEX antenna is not optional on these boards.** They ship with the
  0 Ohm link already set to the u.FL connector, not to the PCB trace - the
  2-pack comes with antennas because you are expected to fit them. Run one
  without and it transmits into an unmatched load: two rooms from the AP that
  measured **-80 dBm**, and a 16 KB JPEG would start arriving and stall at
  around 7 KB. Fitting the antenna took the same spot to **-46 dBm** and the
  stream from 0.2 fps to 6.4 fps. Nothing was soldered; the link was already
  where it needed to be.

  Worth knowing because the failure does not look like an antenna. Small
  responses were fine throughout - `/healthz` answered in 60-100 ms and ping
  showed 0% loss - so the board looked healthy while only bulk transfers died.
  A marginal link fails on sustained throughput first.

* **720p is the ceiling, and it is the JPEG encoder, not the link.** Measured
  on this board at quality 10:

      800x600   11.7 fps   1733 kbit/s
      1024x768   7.5 fps   1954 kbit/s
      1280x720   6.5 fps   1935 kbit/s   <- default
      1280x1024  3.6 fps   1530 kbit/s
      1600x1200  1.3 fps    874 kbit/s

  Delivered throughput *peaks* at 720p and falls off above it - the larger
  sizes cost frames and hand back nothing. `esp_camera_init` is called at UXGA
  regardless, because the driver sizes its frame buffers once and asking for a
  bigger frame later gets you a buffer that cannot hold it; `setup()` steps
  down to the working size immediately afterwards. Without that step-down the
  board boots at 1.2 fps until something calls `/set`.

* **Read `rssi` from `/healthz` in the final mounting spot**, not on the bench.
  If this ends up inside the printer's frame the metal will cost more dB than
  the antenna gains.
* **A camera that will not init** is nearly always the ribbon seated badly or a
  5 V rail that sags when the sensor starts. `setup()` says so and reboots
  rather than serving black frames.

## Build

    cp wifi_creds.h.example wifi_creds.h   # then fill it in; it is gitignored
    arduino-cli compile --fqbn esp32:esp32:esp32cam .
    arduino-cli upload  --fqbn esp32:esp32:esp32cam -p /dev/cu.usbserial-210 .

Serial is 115200 and prints the SSID, IP and RSSI once it is up. Then open
`http://<ip>/` and tune with `/set` rather than reflashing - whatever you
settle on goes into the defaults at the top of the sketch.

Focus is not a setting. The OV2640's lens is a threaded barrel you turn by
hand, frequently with threadlock on it from the factory, and it wants setting
at the distance the camera will actually sit from the plate.

Built against esp32 core 3.3.11. Uses ~35% of the default `huge_app`
partition, so there is room if this ever needs OTA.
