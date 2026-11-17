// CSI RADAR V3 - ESP32-S3 CAM MASTER (AP + dashboard + CSI + detection). Arduino-ESP32 core 3.x.
// Runs on any ESP32-S3 CAM board. The camera is NOT used and NOT initialised, so no camera pins are touched.
// Extra: motion starts SD recording (AVI), stops after 5 min without motion; STREAM button = live view.
// Node name stays "S3" so the DEVKIT and C3 node firmware work unchanged.
// NOT compiled/tested by the author - see README.
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>
#include <math.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Preferences.h>

#define AP_SSID "CSI-RADAR-V3"
#define AP_PASS "RADARV3-123"
#define AP_CH   6
#define UDP_PORT 4210
#define CAL_N   50           // 50 samples @10Hz = 5 s calibration per link
#define NODE_TIMEOUT 2000
#define LINK_TIMEOUT 1000

static const char* NN[3] = {"S3", "DEVKIT", "C3"};
static const uint8_t BC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
typedef struct __attribute__((packed)) { uint8_t magic, id; uint32_t seq, ms; } Beacon;

struct Link {
  float ema, mean, m2, margin, thr, metric, score; uint16_t n; bool seen, motion; uint8_t consec;
  uint32_t lastRx, motionUntil, cool, lastSeq, lost, pkts; int rssi, fps;
};
struct Node { uint32_t lastRx, seq, heap; int rssi, tx, rx; bool rssiOk, hasInfo; };
static Link lk[3][3]; static Node nd[3];
static uint32_t events = 0, lastMotion = 0; static bool everMotion = false;
static String logBuf[10]; static int logN = 0;
static volatile int sens = 7;   // 1..10, higher = more sensitive
static float marginOf(const Link& L) {
  float ks = 6.0f - 0.5f * sens, kr = 0.30f - 0.025f * sens;
  float m = ks * L.margin; if (m < kr * L.mean) m = kr * L.mean; if (m < 0.002f) m = 0.002f; return m;
}

// own CSI aggregation (S3 receives DEVKIT->S3 and C3->S3)
static uint8_t macs[3][6]; static volatile bool known[3];
static volatile uint32_t cnt[3]; static float sumd[3]; static int32_t sumr[3];
static float prevN[3][64]; static bool prevOk[3];
static volatile uint32_t rxCount = 0, txCount = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static WiFiUDP udp; static WebServer server(80);
static uint32_t bSeq = 0, ownSeq = 0, lastRep = 0, lastSec = 0, lastSlot = 0xFFFFFFFF;

static void addLog(const String& s) {
  String t = "T+" + String(millis() / 1000) + "s  " + s;
  if (logN < 10) logBuf[logN++] = t; else { for (int i = 1; i < 10; i++) logBuf[i-1] = logBuf[i]; logBuf[9] = t; }
}

static void csiCb(void*, wifi_csi_info_t* info) {
  if (!info || !info->buf) return;
  int s = -1;
  for (int i = 1; i < 3; i++) if (known[i] && !memcmp(macs[i], info->mac, 6)) { s = i; break; }
  if (s < 0) return;
  int n = info->len / 2; if (n > 64) n = 64; if (n < 8) return;
  float a[64], sum = 0; const int8_t* b = info->buf;
  for (int i = 0; i < n; i++) { float im = b[2*i], re = b[2*i+1]; a[i] = sqrtf(re*re + im*im); sum += a[i]; }
  float mean = sum / n; if (mean < 1.0f) return;
  float d = 0;
  for (int i = 0; i < n; i++) { float v = a[i] / mean; if (prevOk[s]) d += fabsf(v - prevN[s][i]); prevN[s][i] = v; }
  if (prevOk[s]) {
    portENTER_CRITICAL(&mux);
    sumd[s] += d / n; sumr[s] += info->rx_ctrl.rssi; cnt[s]++;
    portEXIT_CRITICAL(&mux);
  }
  prevOk[s] = true;
}

static void onRecv(const esp_now_recv_info_t* ri, const uint8_t* d, int len) {
  if (len != (int)sizeof(Beacon)) return;
  const Beacon* b = (const Beacon*)d;
  if (b->magic != 0xC5 || b->id == 0 || b->id > 2) return;
  memcpy(macs[b->id], ri->src_addr, 6); known[b->id] = true; rxCount++;
}

// ---- detection core: calibration -> adaptive threshold -> 3-sample confirmation -> cooldown
static void onLink(int s, int d, uint32_t seq, float m, int rssi, int fps) {
  Link& L = lk[s][d]; uint32_t now = millis();
  if (L.seen && seq > L.lastSeq) L.lost += seq - L.lastSeq - 1;
  L.lastSeq = seq; L.pkts++; L.lastRx = now; L.rssi = rssi; L.fps = fps; L.metric = m;
  L.ema = L.seen ? 0.5f * L.ema + 0.5f * m : m; L.seen = true;
  if (L.n < CAL_N) {
    L.n++; float dl = m - L.mean; L.mean += dl / L.n; L.m2 += dl * (m - L.mean);
    if (L.n == CAL_N) {
      L.margin = sqrtf(L.m2 / (CAL_N - 1)); L.thr = L.mean + marginOf(L); addLog(String("Calibrated ") + NN[s] + ">" + NN[d]);
    }
    return;
  }
  float mg = marginOf(L); L.thr = L.mean + mg; L.score = (L.ema - L.mean) / mg; if (L.score < 0) L.score = 0;
  if (L.ema > L.thr) { if (L.consec < 255) L.consec++; } else L.consec = 0;
  if (!L.motion) {
    if (L.consec >= (sens >= 7 ? 2 : 3) && now > L.cool) {
      L.motion = true; L.motionUntil = now + 1500; events++; lastMotion = now; everMotion = true;
      addLog(String("MOTION ") + NN[s] + ">" + NN[d]);
    } else if (L.consec == 0) { L.mean += 0.002f * (L.ema - L.mean); }
  } else {
    if (L.ema > L.thr) { L.motionUntil = now + 1500; lastMotion = now; }
    else if (now > L.motionUntil) { L.motion = false; L.cool = now + 1000; }
  }
}

static int nameId(const char* n) { for (int i = 0; i < 3; i++) if (!strcmp(n, NN[i])) return i; return -1; }

static void parseLine(char* line) {
  char a[16], b[16]; unsigned long seq; float m; int rssi, fps, phy, tx, rx; unsigned long heap;
  if (!strncmp(line, "LINK,", 5)) {
    if (sscanf(line, "LINK,%15[^,],%15[^,],%lu,%f,%d,%d", a, b, &seq, &m, &rssi, &fps) == 6) {
      int d = nameId(a), s = nameId(b);
      if (d > 0 && s >= 0 && s != d && isfinite(m) && m >= 0 && m < 10) onLink(s, d, seq, m, rssi, fps);  // reject invalid
    }
  } else if (!strncmp(line, "NODE,", 5)) {
    int n = sscanf(line, "NODE,%15[^,],%lu,%f,%d,%d,%d,%d,%lu", a, &seq, &m, &rssi, &phy, &tx, &rx, &heap);
    int id = nameId(a); if (id <= 0 || n < 5) return;
    Node& N = nd[id]; N.lastRx = millis(); N.seq = seq; N.rssi = rssi; N.rssiOk = true;
    if (n >= 8) { N.tx = tx; N.rx = rx; N.heap = heap; N.hasInfo = true; }
  }
}

static void recalibrate() {
  for (int s = 0; s < 3; s++) for (int d = 0; d < 3; d++) { Link& L = lk[s][d]; L.n = 0; L.mean = 0; L.m2 = 0; L.motion = false; L.consec = 0; L.score = 0; L.seen = false; L.lost = 0; L.pkts = 0; }
  addLog("Recalibration started");
}

static bool linkActive(int s, int d) { return lk[s][d].seen && millis() - lk[s][d].lastRx < LINK_TIMEOUT; }
static int linkState(int s, int d) {  // 0 no data, 1 calibrating, 2 stable, 3 motion, 4 unstable
  Link& L = lk[s][d]; if (!linkActive(s, d)) return 0; if (L.n < CAL_N) return 1;
  uint32_t tot = L.pkts + L.lost; if (tot > 20 && L.lost * 100 / tot > 30) return 4;
  return L.motion ? 3 : 2;
}

// ===================== CAMERA / SD RECORDING / STREAM =====================
// Camera pins: Freenove / ESP32-S3-EYE compatible (most generic "ESP32-S3 CAM" N16R8 boards).
// If your board is different, only change the CAM_PIN_* and CAMSD_* lines below.
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  15
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5
#define CAM_PIN_D7    16
#define CAM_PIN_D6    17
#define CAM_PIN_D5    18
#define CAM_PIN_D4    12
#define CAM_PIN_D3    10
#define CAM_PIN_D2     8
#define CAM_PIN_D1     9
#define CAM_PIN_D0    11
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
#define CAM_PIN_PCLK  13
#define CAMSD_CLK 39      // SD card on SD_MMC, 1-bit mode
#define CAMSD_CMD 38
#define CAMSD_D0  40

#define REC_DIR      "/rec"
#define REC_FPS      10            // recording frame rate
#define STREAM_FPS   15            // live view frame rate
#define REC_HOLD_MS  300000UL      // keep recording until 5 minutes without any motion
#define SEG_MS       600000UL      // start a new file every 10 minutes of recording
#define MAX_FRAMES   7000          // safety limit per file
#define MIN_FREE_MB  50
#define SBUF_CAP     150000        // max JPEG size shared with the stream

static volatile bool camOk = false, sdOk = false, recReq = false, recActive = false;
static volatile int streamClients = 0;
static volatile uint8_t recErr = 0;       // 0 ok, 1 no SD, 2 SD full, 3 file error, 4 write error
static volatile uint32_t recRetryAt = 0, sdFreeMB = 0, aviFrames = 0;
static uint32_t lastMotionSeen = 0, streamBlockUntil = 0;
static Preferences prefs;
static File aviF;
static uint32_t aviMovi = 0, aviMax = 0, aviW = 0, aviH = 0, segStart = 0, fileIdx = 0;
static uint32_t* aviIdx = NULL;
static char curName[24] = "";
static uint8_t* sBuf = NULL; static volatile size_t sLen = 0; static volatile uint32_t sId = 0;
static SemaphoreHandle_t sMux = NULL;
static httpd_handle_t hCam = NULL;

static void put32(uint8_t*& p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; p += 4; }
static void put16(uint8_t*& p, uint16_t v) { p[0] = v; p[1] = v >> 8; p += 2; }
static void putTag(uint8_t*& p, const char* t) { memcpy(p, t, 4); p += 4; }

static bool validName(const char* f) {
  size_t n = strlen(f);
  if (n < 9 || n > 30 || strncmp(f, "vid_", 4) != 0 || strcmp(f + n - 4, ".avi") != 0) return false;
  for (size_t i = 0; i < n; i++) { char c = f[i]; if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) return false; }
  return strstr(f, "..") == NULL;
}

static void updateSdFree() {
  if (!sdOk) return;
  uint64_t t = SD_MMC.totalBytes(), u = SD_MMC.usedBytes();
  sdFreeMB = (t > u) ? (uint32_t)((t - u) / 1048576ULL) : 0;
}

static void sdInit() {
  SD_MMC.setPins(CAMSD_CLK, CAMSD_CMD, CAMSD_D0);
  if (!SD_MMC.begin("/sdcard", true) || SD_MMC.cardType() == CARD_NONE) { Serial.println("[SD] card not found"); return; }
  if (!SD_MMC.exists(REC_DIR)) SD_MMC.mkdir(REC_DIR);
  sdOk = true; updateSdFree();
  Serial.printf("[SD] ok, free %u MB\n", (unsigned)sdFreeMB);
}

static void camInit() {
  if (!psramFound()) { Serial.println("[CAM] PSRAM not found - set Tools > PSRAM (OPI PSRAM)"); return; }
  sBuf = (uint8_t*)ps_malloc(SBUF_CAP);
  aviIdx = (uint32_t*)ps_malloc(MAX_FRAMES * 8);
  if (!sBuf || !aviIdx) { Serial.println("[CAM] buffer alloc failed"); return; }
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = CAM_PIN_D0; c.pin_d1 = CAM_PIN_D1; c.pin_d2 = CAM_PIN_D2; c.pin_d3 = CAM_PIN_D3;
  c.pin_d4 = CAM_PIN_D4; c.pin_d5 = CAM_PIN_D5; c.pin_d6 = CAM_PIN_D6; c.pin_d7 = CAM_PIN_D7;
  c.pin_xclk = CAM_PIN_XCLK; c.pin_pclk = CAM_PIN_PCLK; c.pin_vsync = CAM_PIN_VSYNC; c.pin_href = CAM_PIN_HREF;
  c.pin_sccb_sda = CAM_PIN_SIOD; c.pin_sccb_scl = CAM_PIN_SIOC;
  c.pin_pwdn = CAM_PIN_PWDN; c.pin_reset = CAM_PIN_RESET;
  c.xclk_freq_hz = 20000000; c.pixel_format = PIXFORMAT_JPEG; c.frame_size = FRAMESIZE_VGA;
  c.jpeg_quality = 12; c.fb_count = 2; c.fb_location = CAMERA_FB_IN_PSRAM; c.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) { Serial.printf("[CAM] init failed 0x%x - check camera pins / PSRAM\n", (unsigned)e); return; }
  camOk = true; Serial.println("[CAM] ready (VGA JPEG)");
}

// ---- AVI (MJPEG) writer: 224-byte header, frames, idx1 at the end; header is rewritten when closing
static void aviHeader(uint8_t* h, uint32_t frames, uint32_t us, uint32_t movi, uint32_t fsize, uint32_t maxf) {
  uint8_t* p = h;
  putTag(p, "RIFF"); put32(p, fsize - 8); putTag(p, "AVI ");
  putTag(p, "LIST"); put32(p, 192); putTag(p, "hdrl");
  putTag(p, "avih"); put32(p, 56);
  put32(p, us); put32(p, 0); put32(p, 0); put32(p, 0x10); put32(p, frames); put32(p, 0); put32(p, 1); put32(p, maxf);
  put32(p, aviW); put32(p, aviH); put32(p, 0); put32(p, 0); put32(p, 0); put32(p, 0);
  putTag(p, "LIST"); put32(p, 116); putTag(p, "strl");
  putTag(p, "strh"); put32(p, 56);
  putTag(p, "vids"); putTag(p, "MJPG"); put32(p, 0); put32(p, 0); put32(p, 0);
  put32(p, us); put32(p, 1000000); put32(p, 0); put32(p, frames); put32(p, maxf); put32(p, 0xFFFFFFFF); put32(p, 0);
  put16(p, 0); put16(p, 0); put16(p, aviW); put16(p, aviH);
  putTag(p, "strf"); put32(p, 40);
  put32(p, 40); put32(p, aviW); put32(p, aviH); put16(p, 1); put16(p, 24); putTag(p, "MJPG");
  put32(p, aviW * aviH * 3); put32(p, 0); put32(p, 0); put32(p, 0); put32(p, 0);
  putTag(p, "LIST"); put32(p, movi + 4); putTag(p, "movi");
}

static void recClose() {
  if (!recActive) return;
  uint32_t n = aviFrames, idxBytes = n * 16;
  uint8_t t[8]; uint8_t* p = t; putTag(p, "idx1"); put32(p, idxBytes); aviF.write(t, 8);
  uint8_t e[16 * 64];
  for (uint32_t i = 0; i < n;) {
    uint32_t k = n - i; if (k > 64) k = 64;
    uint8_t* q = e;
    for (uint32_t j = 0; j < k; j++, i++) { putTag(q, "00dc"); put32(q, 0x10); put32(q, aviIdx[2 * i]); put32(q, aviIdx[2 * i + 1]); }
    aviF.write(e, k * 16);
  }
  uint32_t ms = millis() - segStart;
  uint32_t us = n ? (uint32_t)((uint64_t)ms * 1000ULL / n) : 100000;
  if (us < 20000) us = 20000;
  uint8_t hdr[224]; aviHeader(hdr, n, us, aviMovi, 224 + aviMovi + 8 + idxBytes, aviMax);
  aviF.seek(0); aviF.write(hdr, 224); aviF.close();
  recActive = false; updateSdFree();
}

static void recOpen(camera_fb_t* fb) {
  uint32_t now = millis();
  if (!sdOk) { recErr = 1; recRetryAt = now + 10000; return; }
  if (sdFreeMB < MIN_FREE_MB) { recErr = 2; recRetryAt = now + 30000; return; }
  fileIdx++; prefs.putUInt("idx", fileIdx);
  snprintf(curName, sizeof(curName), "vid_%05u.avi", (unsigned)fileIdx);
  aviF = SD_MMC.open(String(REC_DIR) + "/" + curName, FILE_WRITE);
  if (!aviF) { recErr = 3; recRetryAt = now + 10000; return; }
  uint8_t hdr[224]; memset(hdr, 0, sizeof(hdr)); aviF.write(hdr, 224);
  aviFrames = 0; aviMovi = 0; aviMax = 0; aviW = fb->width; aviH = fb->height; segStart = now;
  recActive = true; recErr = 0;
}

static void recAppend(camera_fb_t* fb) {
  if (aviFrames >= MAX_FRAMES || millis() - segStart >= SEG_MS) { recClose(); return; }   // next frame opens a new file
  uint32_t sz = fb->len; uint8_t ch[8]; uint8_t* p = ch; putTag(p, "00dc"); put32(p, sz);
  size_t w = aviF.write(ch, 8); w += aviF.write(fb->buf, sz);
  if (sz & 1) { uint8_t z = 0; w += aviF.write(&z, 1); }
  if (w != 8 + sz + (sz & 1)) { recErr = 4; recClose(); recRetryAt = millis() + 10000; return; }
  aviIdx[2 * aviFrames] = 4 + aviMovi; aviIdx[2 * aviFrames + 1] = sz; aviFrames++;
  aviMovi += 8 + sz + (sz & 1); if (sz > aviMax) aviMax = sz;
}

// camera task: only grabs frames while recording or streaming. Otherwise idle = monitoring only.
static void camTask(void*) {
  uint32_t last = 0;
  for (;;) {
    bool want = recReq;
    if (!want && recActive) recClose();
    bool strm = streamClients > 0;
    if (!camOk || (!want && !strm)) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    uint32_t now = millis(), iv = want ? (1000 / REC_FPS) : (1000 / STREAM_FPS);
    if (now - last < iv) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    last = now;
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    if (want) { if (!recActive && now >= recRetryAt) recOpen(fb); if (recActive) recAppend(fb); }
    if (strm && fb->len <= SBUF_CAP && xSemaphoreTake(sMux, pdMS_TO_TICKS(30))) {
      memcpy(sBuf, fb->buf, fb->len); sLen = fb->len; sId++; xSemaphoreGive(sMux);
    }
    esp_camera_fb_return(fb);
  }
}

// ---- HTTP on port 81: /stream (MJPEG live view) and /dl?f=NAME (download a recording)
static esp_err_t streamHandler(httpd_req_t* req) {
  if (!camOk) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not ready"); return ESP_FAIL; }
  uint8_t* local = (uint8_t*)ps_malloc(SBUF_CAP);
  if (!local) return ESP_FAIL;
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  streamClients++;
  uint32_t lastId = sId, lastChange = millis(); esp_err_t res = ESP_OK; char part[96];
  while (res == ESP_OK) {
    if (sId == lastId) {
      if (millis() - lastChange > 5000) break;
      vTaskDelay(pdMS_TO_TICKS(10)); continue;
    }
    size_t len = 0;
    if (xSemaphoreTake(sMux, pdMS_TO_TICKS(100))) { len = sLen; memcpy(local, sBuf, len); lastId = sId; xSemaphoreGive(sMux); }
    if (!len) continue;
    lastChange = millis();
    int h = snprintf(part, sizeof(part), "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
    res = httpd_resp_send_chunk(req, part, h);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)local, len);
  }
  streamClients--; free(local);
  return res;
}

static esp_err_t dlHandler(httpd_req_t* req) {
  char q[96], f[40];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "f", f, sizeof(f)) != ESP_OK || !validName(f)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_FAIL;
  }
  if (recActive && !strcmp(f, curName)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file is being recorded"); return ESP_FAIL; }
  File fl = SD_MMC.open(String(REC_DIR) + "/" + f, FILE_READ);
  if (!fl) { httpd_resp_send_404(req); return ESP_FAIL; }
  uint8_t* buf = (uint8_t*)ps_malloc(16384);
  if (!buf) { fl.close(); return ESP_FAIL; }
  char cd[80]; snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", f);
  httpd_resp_set_type(req, "video/x-msvideo"); httpd_resp_set_hdr(req, "Content-Disposition", cd);
  esp_err_t res = ESP_OK;
  for (;;) {
    size_t n = fl.read(buf, 16384); if (!n) break;
    res = httpd_resp_send_chunk(req, (const char*)buf, n); if (res != ESP_OK) break;
  }
  if (res == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
  fl.close(); free(buf);
  return res;
}

static void startCamServer() {
  sMux = xSemaphoreCreateMutex();
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 81; c.ctrl_port = 32769; c.max_open_sockets = 3; c.stack_size = 8192; c.lru_purge_enable = true;
  httpd_uri_t u1 = {}; u1.uri = "/stream"; u1.method = HTTP_GET; u1.handler = streamHandler;
  httpd_uri_t u2 = {}; u2.uri = "/dl"; u2.method = HTTP_GET; u2.handler = dlHandler;
  if (httpd_start(&hCam, &c) == ESP_OK) { httpd_register_uri_handler(hCam, &u1); httpd_register_uri_handler(hCam, &u2); Serial.println("[CAM] stream server on :81"); }
  else Serial.println("[ERR] camera http server failed");
  xTaskCreatePinnedToCore(camTask, "cam", 10240, NULL, 1, NULL, 0);
}

static String camJson() {
  uint32_t now = millis(); long stop = -1;
  if (recReq && streamClients == 0) { long r = (long)(REC_HOLD_MS - (now - lastMotionSeen)); stop = r > 0 ? r / 1000 : 0; }
  String s = String(",\"cam\":{\"ok\":") + (camOk ? 1 : 0) + ",\"sd\":" + (sdOk ? 1 : 0) + ",\"free\":" + (unsigned long)sdFreeMB +
             ",\"req\":" + (recReq ? 1 : 0) + ",\"rec\":" + (recActive ? 1 : 0) + ",\"err\":" + (int)recErr +
             ",\"strm\":" + (streamClients > 0 ? 1 : 0) + ",\"stop\":" + stop + ",\"fr\":" + (unsigned long)aviFrames +
             ",\"name\":\"" + (recActive ? curName : "") + "\"}";
  return s;
}

static void handleFiles() {
  String j = "{\"files\":[";
  if (sdOk) {
    File d = SD_MMC.open(REC_DIR);
    if (d) {
      File f = d.openNextFile(); int n = 0; bool first = true;
      while (f && n < 60) {
        if (!f.isDirectory()) {
          String nm = f.name();
          if (validName(nm.c_str())) { if (!first) j += ","; first = false; j += String("{\"n\":\"") + nm + "\",\"s\":" + (unsigned long)f.size() + ",\"cur\":" + ((recActive && nm == curName) ? 1 : 0) + "}"; n++; }
        }
        f = d.openNextFile();
      }
      d.close();
    }
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void handleDel() {
  String f = server.arg("f");
  if (sdOk && validName(f.c_str()) && !(recActive && f == curName)) { SD_MMC.remove(String(REC_DIR) + "/" + f); updateSdFree(); }
  server.send(200, "text/plain", "ok");
}

static void handleData() {
  uint32_t now = millis(); nd[0].lastRx = now;
  int active = 0, cal = 0, mot = 0, unst = 0, stab = 0, discon = 0; float an = 0; float calSum = 0; int calCnt = 0;
  for (int i = 1; i < 3; i++) if (now - nd[i].lastRx > NODE_TIMEOUT || nd[i].lastRx == 0) discon++;
  String j; j.reserve(2400);
  String links = "[";
  for (int s = 0; s < 3; s++) for (int d = 0; d < 3; d++) {
    if (s == d) continue; Link& L = lk[s][d]; int st = linkState(s, d);
    if (st) active++; if (st == 1) cal++; if (st == 3) mot++; if (st == 4) unst++; if (st == 2) stab++;
    if (L.seen) { calSum += (L.n >= CAL_N ? 1.0f : (float)L.n / CAL_N); calCnt++; }
    if (st >= 2 && L.score * 100 > an) an = L.score * 100;
    uint32_t tot = L.pkts + L.lost;
    links += String(links.length() > 1 ? "," : "") + "{\"s\":" + s + ",\"d\":" + d + ",\"st\":" + st + ",\"m\":" + String(L.metric, 4) + ",\"t\":" + String(L.thr, 4) +
             ",\"sc\":" + String(L.score, 2) + ",\"fps\":" + L.fps + ",\"loss\":" + String(tot ? L.lost * 100.0f / tot : 0, 1) + ",\"age\":" + (L.seen ? (long)(now - L.lastRx) : -1) + "}";
  }
  links += "]";
  const char* state;
  if (discon == 2) state = "NODE DISCONNECTED";
  else if (active == 0) state = "INSUFFICIENT DATA";
  else if (mot > 0) state = "MOTION DETECTED";
  else if (cal > 0 && stab == 0) state = "CALIBRATING";
  else if (unst > 0) state = "SIGNAL UNSTABLE";
  else if (discon > 0) state = "NODE DISCONNECTED";
  else if (stab < 4) state = "INSUFFICIENT DATA";
  else state = "NO MOTION";
  j = String("{\"state\":\"") + state + "\",\"ev\":" + events + ",\"last\":" + (everMotion ? (long)((now - lastMotion) / 1000) : -1) +
      ",\"cal\":" + (calCnt ? (int)(calSum * 100 / calCnt) : 0) + ",\"up\":" + now / 1000 + ",\"an\":" + String(an > 100 ? 100 : an, 0) + ",\"heap\":" + ESP.getFreeHeap() + camJson() + ",\"nodes\":[";
  for (int i = 0; i < 3; i++) {
    Node& N = nd[i]; bool c = (i == 0) || (N.lastRx && now - N.lastRx < NODE_TIMEOUT);
    j += String(i ? "," : "") + "{\"n\":\"" + NN[i] + "\",\"c\":" + (c ? 1 : 0) + ",\"rssi\":" + (N.rssiOk ? String(N.rssi) : "null") +
         ",\"seq\":" + N.seq + ",\"age\":" + (i == 0 ? 0 : (N.lastRx ? (long)(now - N.lastRx) : -1)) +
         ",\"tx\":" + (i == 0 ? String(N.tx) : (N.hasInfo ? String(N.tx) : "null")) + ",\"rx\":" + (i == 0 ? String(N.rx) : (N.hasInfo ? String(N.rx) : "null")) +
         ",\"heap\":" + ((i == 0 || N.hasInfo) ? String(i == 0 ? ESP.getFreeHeap() : N.heap) : "null") + "}";
  }
  j += "],\"links\":" + links + ",\"log\":[";
  for (int i = logN - 1; i >= 0; i--) j += String(i < logN - 1 ? "," : "") + "\"" + logBuf[i] + "\"";
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>CSI RADAR V3</title>
<style>
:root{--bg:#070b12;--card:#0f1722;--bd:#1c2a3d;--tx:#d6e4f5;--mu:#6f86a3;--ac:#00d8ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:14px system-ui,Segoe UI,Roboto,sans-serif;padding:12px;max-width:1100px;margin:auto}
h1{font-size:16px;letter-spacing:2px;color:var(--ac);margin:4px 0 10px}h2{font-size:12px;letter-spacing:1.5px;color:var(--mu);margin:0 0 8px;font-weight:600}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:12px}.grid{display:grid;gap:10px;margin-bottom:10px}
.g3{grid-template-columns:repeat(auto-fit,minmax(230px,1fr))}.g2{grid-template-columns:repeat(auto-fit,minmax(320px,1fr))}
#banner{font-size:26px;font-weight:700;text-align:center;padding:16px;border-radius:10px;border:1px solid var(--bd);transition:background .2s,color .2s}
.row{display:flex;justify-content:space-between;padding:2px 0;color:var(--mu)}.row b{color:var(--tx);font-weight:500}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;background:#555}
button{background:#12304a;color:var(--tx);border:1px solid var(--ac);border-radius:6px;padding:8px 14px;cursor:pointer;margin-right:6px}button:active{background:#1b4a70}
.bar{height:8px;background:#0a1019;border-radius:4px;overflow:hidden;margin:6px 0}.bar i{display:block;height:100%;background:var(--ac);width:0;transition:width .3s}
.heat{display:flex;gap:8px;justify-content:space-around}.cell{flex:1;text-align:center;padding:18px 4px;border-radius:8px;font-weight:600;transition:background .3s;color:#04101c}
.lk{display:flex;justify-content:space-between;align-items:center;padding:4px 0;border-bottom:1px solid #15202f}.lk:last-child{border:0}
canvas{width:100%;height:130px;display:block}#log div{padding:2px 0;color:var(--mu);font-size:12px}.na{color:#8a6d3b}.small{font-size:11px;color:var(--mu);margin-top:6px}
</style></head><body>
<h1>CSI RADAR OS V3 &middot; TRI-NODE</h1>
<div id="banner">CONNECTING...</div>
<div class="grid g3" style="margin-top:10px">
<div class="card"><h2>MOTION PANEL</h2><div class="row">Events<b id="ev">0</b></div><div class="row">Last motion<b id="last">-</b></div><div class="row">Anomaly score<b id="an">0</b></div>
<div class="row">Calibration<b id="calp">0%</b></div><div class="bar"><i id="calb"></i></div>
<div class="row">Sensitivity<b id="sv">7</b></div><input type="range" id="sl" min="1" max="10" value="7" style="width:100%" oninput="$('sv').textContent=this.value" onchange="fetch('/sens?v='+this.value,{method:'POST'})">
<button onclick="post('/recal')">Recalibrate</button><button onclick="post('/clear')">Clear events</button>
<div class="small">Stay still during calibration (~5 s).</div></div>
<div class="card"><h2>SYSTEM</h2><div class="row">Uptime<b id="up">-</b></div><div class="row">Master free heap<b id="heap">-</b></div><div class="row">Dashboard RTT (measured)<b id="rtt">-</b></div><div class="row">CPU load<b class="na">N/A</b></div><div class="row">Packet latency<b class="na">N/A (no clock sync)</b></div></div>
<div class="card"><h2>OSCILLATION HEATMAP</h2><div class="heat" id="heat"></div><div class="small">Signal-variation only. Not a thermal image, not a person location.</div></div>
</div>
<div class="grid g2"><div class="card"><h2>CAMERA</h2>
<div class="row">Camera<b id="cst">-</b></div><div class="row">Recording<b id="rst">-</b></div><div class="row">Auto-stop in<b id="rstop">-</b></div><div class="row">SD card<b id="sdst">-</b></div>
<button id="sbtn" onclick="toggleStream()">STREAM</button>
<div class="small">While streaming, motion detection is paused. Press STOP STREAM to return to monitoring.</div>
<img id="vid" style="display:none;width:100%;margin-top:8px;border-radius:8px" alt=""></div>
<div class="card"><h2>RECORDINGS (SD)</h2><div id="files">-</div><button onclick="loadFiles()" style="margin-top:8px">Refresh</button></div></div>
<div class="grid g3" id="nodes"></div>
<div class="grid g2"><div class="card"><h2>CSI ANOMALY (live, 0-100)</h2><canvas id="cv" width="600" height="130"></canvas></div>
<div class="card"><h2>DIRECTIONAL LINKS</h2><div id="links"></div></div></div>
<div class="card"><h2>EVENT HISTORY</h2><div id="log"></div></div>
<script>
const NN=['S3','DEVKIT','C3'],COL=['#1e5eff','#00d8ff','#2ee66b','#ffd21f','#ff3b3b'],ST=['NO DATA','CALIBRATING','STABLE','MOTION','UNSTABLE'];
const BC={'MOTION DETECTED':['#5a0f0f','#ff6b6b'],'NO MOTION':['#0c3a1f','#4dff94'],'CALIBRATING':['#3a3410','#ffd21f'],'SIGNAL UNSTABLE':['#4a2a08','#ffa52f'],'NODE DISCONNECTED':['#3a1030','#ff6bd6'],'INSUFFICIENT DATA':['#1c2a3d','#9fb4cf'],'STREAMING - DETECTION PAUSED':['#0d2f4a','#00d8ff']};
const $=i=>document.getElementById(i);let hist=[];
let streaming=false,prevRec=-1;const BLANK='data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==';
function toggleStream(){const im=$('vid');
 if(!streaming){streaming=true;im.onerror=()=>{if(streaming)stopStream()};im.style.display='block';im.src='http://'+location.hostname+':81/stream?'+Date.now();$('sbtn').textContent='STOP STREAM'}
 else stopStream()}
function stopStream(){streaming=false;const im=$('vid');im.onerror=null;im.src=BLANK;im.style.display='none';$('sbtn').textContent='STREAM'}
async function loadFiles(){try{const r=await fetch('/files',{cache:'no-store'});const j=await r.json();const h='http://'+location.hostname+':81/dl?f=';
 $('files').innerHTML=j.files.length?j.files.slice().reverse().map(f=>'<div class="lk"><span>'+f.n+' &middot; '+(f.s/1048576).toFixed(1)+' MB'+(f.cur?' (recording)':'')+'</span><span>'+(f.cur?'':'<a href="'+h+f.n+'" style="color:var(--ac)">Download</a> <a href="#" style="color:#ff6b6b" onclick="del(\''+f.n+'\');return false">Delete</a>')+'</span></div>').join(''):'No recordings'}catch(e){$('files').textContent='Cannot read SD'}}
function del(n){if(confirm('Delete '+n+'?'))fetch('/del?f='+n,{method:'POST'}).then(loadFiles)}
const lvl=s=>s<.25?0:s<.5?1:s<.85?2:s<1.3?3:4;
function post(u){fetch(u,{method:'POST'})}
function fmt(v,u){return v==null?'<span class="na">N/A</span>':v+(u||'')}
function render(j){
 const b=$('banner');const stt=(j.cam&&j.cam.strm)?'STREAMING - DETECTION PAUSED':j.state;b.textContent=stt;const c=BC[stt]||BC['INSUFFICIENT DATA'];b.style.background=c[0];b.style.color=c[1];
 $('ev').textContent=j.ev;$('last').textContent=j.last<0?'never':j.last+' s ago';$('an').textContent=j.an;$('calp').textContent=j.cal+'%';$('calb').style.width=j.cal+'%';
 $('up').textContent=Math.floor(j.up/60)+'m '+j.up%60+'s';$('heap').textContent=Math.round(j.heap/1024)+' KB';
 $('nodes').innerHTML=j.nodes.map(n=>'<div class="card"><h2><span class="dot" style="background:'+(n.c?'#2ee66b':'#ff3b3b')+'"></span>'+n.n+(n.n=='S3'?' (MASTER)':'')+'</h2>'+
  '<div class="row">Status<b>'+(n.c?'CONNECTED':'DISCONNECTED')+'</b></div><div class="row">RSSI (to AP)<b>'+fmt(n.rssi,' dBm')+'</b></div><div class="row">TX beacons/s<b>'+fmt(n.tx)+'</b></div><div class="row">RX beacons/s<b>'+fmt(n.rx)+'</b></div>'+
  '<div class="row">Telemetry age<b>'+(n.age<0?'<span class="na">N/A</span>':n.age+' ms')+'</b></div><div class="row">Seq<b>'+n.seq+'</b></div><div class="row">Free heap<b>'+fmt(n.heap!=null?Math.round(n.heap/1024):null,' KB')+'</b></div></div>').join('');
 const mx=[0,0,0],has=[0,0,0];
 $('links').innerHTML=j.links.map(l=>{if(l.st>=2){[l.s,l.d].forEach(k=>{has[k]=1;if(l.sc>mx[k])mx[k]=l.sc})}
  return '<div class="lk"><span><span class="dot" style="background:'+(l.st==0?'#555':COL[l.st==3?4:l.st==4?3:l.st==1?3:2])+'"></span>'+NN[l.s]+' &rarr; '+NN[l.d]+'</span><span style="color:var(--mu)">'+ST[l.st]+' &middot; '+l.fps+' csi/s &middot; loss '+l.loss+'%</span></div>'}).join('');
 $('heat').innerHTML=NN.map((n,i)=>'<div class="cell" style="background:'+(has[i]?COL[lvl(mx[i])]:'#2a3547')+'">'+n+'</div>').join('');
 $('log').innerHTML=j.log.length?j.log.map(x=>'<div>'+x+'</div>').join(''):'<div>No events</div>';
 const cm=j.cam;if(cm){const E=['','NO SD CARD','SD FULL','FILE ERROR','WRITE ERROR'];
  $('cst').innerHTML=cm.ok?'READY':'<span class="na">NOT READY</span>';
  $('rst').innerHTML=cm.rec?'<span style="color:#ff6b6b">&#9679; RECORDING</span> '+cm.name+' ('+cm.fr+' fr)':(cm.req?'<span class="na">WAITING '+(E[cm.err]||'')+'</span>':'idle (monitoring)');
  $('rstop').textContent=cm.req?(cm.strm?'paused (streaming)':Math.floor(cm.stop/60)+'m '+cm.stop%60+'s'):'-';
  $('sdst').innerHTML=cm.sd?(cm.free+' MB free'):'<span class="na">NOT FOUND</span>';
  if(cm.rec!==prevRec){prevRec=cm.rec;loadFiles()}}
 hist.push(j.an);if(hist.length>120)hist.shift();draw();
}
function draw(){const cv=$('cv'),x=cv.getContext('2d');x.clearRect(0,0,600,130);x.strokeStyle='#1c2a3d';for(let i=0;i<=4;i++){x.beginPath();x.moveTo(0,i*32.5);x.lineTo(600,i*32.5);x.stroke()}
 x.strokeStyle='#00d8ff';x.lineWidth=2;x.beginPath();hist.forEach((v,i)=>{const px=i*5,py=130-v*1.3;i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke();
 x.strokeStyle='#ff3b3b55';x.beginPath();x.moveTo(0,130-100*1.3/1.3*0.77*1.3/1.3);x.stroke()}
async function tick(){const t=performance.now();try{const r=await fetch('/data',{cache:'no-store'});const j=await r.json();$('rtt').textContent=Math.round(performance.now()-t)+' ms';render(j)}catch(e){$('banner').textContent='DASHBOARD OFFLINE';$('banner').style.background='#3a1030'}setTimeout(tick,250)}
tick();
</script></body></html>)HTML";

void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("[S3] CSI RADAR V3 master (ESP32-S3 CAM)");
  Serial.printf("[S3] PSRAM: %s (%u KB)  Flash: %u MB\n", psramFound() ? "found" : "NOT found (check Tools > PSRAM)",
                (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFlashChipSize() / 1048576));
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_CH, 0, 4);
  Serial.printf("[S3] AP up ch%d IP %s\n", AP_CH, WiFi.softAPIP().toString().c_str());
  if (esp_now_init() != ESP_OK) Serial.println("[ERR] esp_now_init");
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, BC, 6); p.channel = 0; p.ifidx = WIFI_IF_AP; p.encrypt = false;
  esp_now_add_peer(&p);
  esp_wifi_config_espnow_rate(WIFI_IF_AP, WIFI_PHY_RATE_6M);
  wifi_csi_config_t c; memset(&c, 0, sizeof(c));
  c.lltf_en = true; c.htltf_en = false; c.stbc_htltf2_en = false; c.ltf_merge_en = false; c.channel_filter_en = false; c.manu_scale = false; c.shift = 0;
  esp_wifi_set_csi_config(&c); esp_wifi_set_csi_rx_cb(csiCb, NULL); esp_wifi_set_csi(true);
  udp.begin(UDP_PORT);
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/data", handleData);
  server.on("/sens", HTTP_POST, []() { if (server.hasArg("v")) { int v = server.arg("v").toInt(); if (v >= 1 && v <= 10) sens = v; } server.send(200, "text/plain", "ok"); });
  server.on("/recal", HTTP_POST, []() { recalibrate(); server.send(200, "text/plain", "ok"); });
  server.on("/clear", HTTP_POST, []() { events = 0; everMotion = false; logN = 0; server.send(200, "text/plain", "ok"); });
  server.on("/files", handleFiles);
  server.on("/del", HTTP_POST, handleDel);
  server.begin();
  prefs.begin("rec", false); fileIdx = prefs.getUInt("idx", 0);
  sdInit(); camInit(); startCamServer();
  addLog("Master started");
}

void loop() {
  uint32_t now = millis();
  server.handleClient();
  int sz;
  while ((sz = udp.parsePacket()) > 0) {
    char buf[200]; int n = udp.read(buf, sizeof(buf) - 1); if (n <= 0) break; buf[n] = 0;
    char* line = strtok(buf, "\n"); while (line) { parseLine(line); line = strtok(NULL, "\n"); }
  }
  uint32_t slot = now / 10;   // master defines the TDM clock: slot 0 of each 30 ms frame
  if (slot != lastSlot) { lastSlot = slot; if (slot % 3 == 0) { Beacon b = {0xC5, 0, bSeq++, now}; if (esp_now_send(BC, (uint8_t*)&b, sizeof(b)) == ESP_OK) txCount++; } }
  // ---- motion-triggered recording. Start on motion, stop after 5 min with no motion.
  // While streaming: detection is paused and the stop timer is frozen, so the stream traffic cannot trigger anything.
  {
    bool mot = false;
    for (int s = 0; s < 3; s++) for (int d = 0; d < 3; d++) if (s != d && lk[s][d].motion && linkActive(s, d)) mot = true;
    if (streamClients > 0) { streamBlockUntil = now + 3000; if (recReq) lastMotionSeen = now; }
    else if (now > streamBlockUntil) {
      if (mot) { lastMotionSeen = now; if (!recReq) { recReq = true; addLog("REC START (motion)"); } }
      else if (recReq && now - lastMotionSeen >= REC_HOLD_MS) { recReq = false; addLog("REC STOP (5 min no motion)"); }
    }
    static bool prevAct = false;
    if (recActive != prevAct) { prevAct = recActive; addLog(recActive ? String("Recording ") + curName : String("Recording file closed")); }
  }
  if (now - lastSec >= 1000) { lastSec = now; nd[0].tx = txCount; nd[0].rx = rxCount; txCount = 0; rxCount = 0; nd[0].hasInfo = true; }
  if (now - lastRep >= 100) {
    lastRep = now; ownSeq++;
    for (int s = 1; s < 3; s++) {
      portENTER_CRITICAL(&mux);
      uint32_t c = cnt[s]; float m = c ? sumd[s] / c : 0; int r = c ? sumr[s] / (int)c : 0;
      cnt[s] = 0; sumd[s] = 0; sumr[s] = 0;
      portEXIT_CRITICAL(&mux);
      if (c) onLink(s, 0, ownSeq, m, r, c * 10);
    }
  }
  delay(1);
}
