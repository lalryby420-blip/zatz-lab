// CSI RADAR V3 - NODE (C3 / same code as C3 firmware)
// Arduino-ESP32 core 3.x. NOT compiled/tested by the author - see README.
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>
#include <math.h>

#define NODE_ID   1          // 0=S3 1=DEVKIT 2=C3
#define NODE_NAME "DEVKIT"
#define AP_SSID   "CSI-RADAR-V3"
#define AP_PASS   "RADARV3-123"
#define AP_CH     6
#define MASTER_IP "192.168.4.1"
#define UDP_PORT  4210

static const char* NN[3] = {"S3", "DEVKIT", "C3"};
static const uint8_t BC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
typedef struct __attribute__((packed)) { uint8_t magic, id; uint32_t seq, ms; } Beacon;

static uint8_t macs[3][6];
static volatile bool known[3];
static volatile uint32_t cnt[3];
static float sumd[3]; static int32_t sumr[3];
static float prevN[3][64]; static bool prevOk[3];
static volatile int32_t tOff = 0;
static volatile uint32_t rxCount = 0, txCount = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static WiFiUDP udp;
static uint32_t seqNo = 0, bSeq = 0, lastRep = 0, lastSec = 0, lastSlot = 0xFFFFFFFF, lastTry = 0;
static uint32_t txfps = 0, rxfps = 0;

// ---- CSI: per-packet normalized amplitude vector; metric = mean |delta| vs previous packet (gain-invariant)
static void csiCb(void*, wifi_csi_info_t* info) {
  if (!info || !info->buf) return;
  int s = -1;
  for (int i = 0; i < 3; i++) if (i != NODE_ID && known[i] && !memcmp(macs[i], info->mac, 6)) { s = i; break; }
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
  if (b->magic != 0xC5 || b->id > 2 || b->id == NODE_ID) return;
  memcpy(macs[b->id], ri->src_addr, 6); known[b->id] = true; rxCount++;
  if (b->id == 0) tOff = (int32_t)(b->ms - millis());   // sync TDM clock to master
}

static void csiInit() {
  wifi_csi_config_t c; memset(&c, 0, sizeof(c));
  c.lltf_en = true; c.htltf_en = false; c.stbc_htltf2_en = false; c.ltf_merge_en = false;
  c.channel_filter_en = false; c.manu_scale = false; c.shift = 0;
  esp_wifi_set_csi_config(&c);
  esp_wifi_set_csi_rx_cb(csiCb, NULL);
  esp_wifi_set_csi(true);
}

static void radioInit() {
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setAutoReconnect(true);
  WiFi.begin(AP_SSID, AP_PASS, AP_CH);
  uint32_t t = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(100);
  if (esp_now_init() != ESP_OK) { Serial.println("[ERR] esp_now_init"); return; }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t p; memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, BC, 6); p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = false;
  esp_now_add_peer(&p);
  esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_6M);  // OFDM frame so CSI exists (1M DSSS gives no CSI)
  csiInit();
}

void setup() {
  Serial.begin(115200); delay(300);
  Serial.printf("[%s] CSI RADAR V3 node id=%d\n", NODE_NAME, NODE_ID);
  radioInit();
  udp.begin(UDP_PORT + 1 + NODE_ID);
}

void loop() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastTry > 3000) { lastTry = now; Serial.println("[WARN] WiFi lost, reconnecting"); WiFi.disconnect(); WiFi.begin(AP_SSID, AP_PASS, AP_CH); }
    delay(20); return;
  }
  // TDM: 10ms slots, 3 slots/frame, slot index == NODE_ID, clock aligned to master
  uint32_t slot = (now + (uint32_t)tOff) / 10;
  if (slot != lastSlot) {
    lastSlot = slot;
    if (slot % 3 == NODE_ID) { Beacon b = {0xC5, NODE_ID, bSeq++, now}; if (esp_now_send(BC, (uint8_t*)&b, sizeof(b)) == ESP_OK) txCount++; }
  }
  if (now - lastSec >= 1000) { lastSec = now; txfps = txCount; rxfps = rxCount; txCount = 0; rxCount = 0; }
  if (now - lastRep >= 100) {
    lastRep = now; seqNo++;
    float mx = 0; int rs = WiFi.RSSI(); char buf[160];
    for (int s = 0; s < 3; s++) {
      if (s == NODE_ID) continue;
      portENTER_CRITICAL(&mux);
      uint32_t c = cnt[s]; float m = c ? sumd[s] / c : 0; int r = c ? sumr[s] / (int)c : 0;
      cnt[s] = 0; sumd[s] = 0; sumr[s] = 0;
      portEXIT_CRITICAL(&mux);
      if (!c) continue;
      snprintf(buf, sizeof(buf), "LINK,%s,%s,%lu,%.5f,%d,%lu\n", NODE_NAME, NN[s], (unsigned long)seqNo, m, r, (unsigned long)(c * 10));
      udp.beginPacket(MASTER_IP, UDP_PORT); udp.print(buf); udp.endPacket();
      if (m > mx) mx = m;
    }
    // compat line + extras (tx,rx,heap). phy_mbps=0 means N/A
    snprintf(buf, sizeof(buf), "NODE,%s,%lu,%.5f,%d,0,%lu,%lu,%lu\n", NODE_NAME, (unsigned long)seqNo, mx, rs, (unsigned long)txfps, (unsigned long)rxfps, (unsigned long)ESP.getFreeHeap());
    udp.beginPacket(MASTER_IP, UDP_PORT); udp.print(buf); udp.endPacket();
  }
  delay(1);
}
