/*
  ESP32-S3 Fake 3G Modem — المرحلة 1: نتجسسوا على الرسيفر
  ---------------------------------------------------------
  الـ ESP32-S3 يتظاهر بأنه مودم 3G (Huawei) على منفذ الـ USB متاع الرسيفر،
  ويرد على أوامر AT، ولما الرسيفر يتصل (ATD*99#) يرد CONNECT
  ويطبع كل البايتات اللي يبعتها الرسيفر (لازم تكون فريمات PPP تبدا بـ 7E).

  الهدف: نعرفوا هل الرسيفر يتعرّف على الـ S3 وشن الأوامر اللي يبعتها.
  لو شفنا فريمات PPP، ننتقلوا للمرحلة 2 (PPP + NAT على الوايفاي).

  إعدادات Arduino IDE (Tools):
    Board            : ESP32S3 Dev Module
    USB Mode         : USB-OTG (TinyUSB)
    USB CDC On Boot  : Disabled      <-- مهم: باش Serial يطلع على منفذ الـ COM
    Flash Size       : 16MB
    PSRAM            : OPI PSRAM
  الرفع (Upload) واللوق (Serial Monitor 115200) من منفذ الـ COM (اللي فيه شيب CH343/CP2102).
  منفذ الـ USB الأصلي (OTG) يتوصّل للرسيفر.
*/

#include "USB.h"
#include "USBCDC.h"
#include <WiFi.h>

// ===== وايفاي الدار (نحتاجوه في المرحلة 2، توا نجربوا الاتصال بس) =====
const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
bool wifiReported = false;

// ===== اختار الهوية اللي يشوفها الرسيفر =====
// 0 = Espressif عادي (CDC)
// 1 = Huawei E1550 / E169   (12D1:1001)
// 2 = Huawei modem mode     (12D1:1506)
#define PROFILE 1

USBCDC modem;

volatile bool dataMode = false;
bool echoOn = true;
String lineBuf;
uint32_t dataBytes = 0;
uint32_t lastDataMs = 0;

// ---------- أدوات الرد ----------
void sendRaw(const String &s) {
  modem.print("\r\n");
  modem.print(s);
  modem.print("\r\n");
  modem.flush();
  Serial.printf("   << %s\n", s.c_str());
}
void ok() { sendRaw("OK"); }

// ---------- معالجة أوامر AT ----------
void handleAT(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  String u = cmd;
  u.toUpperCase();
  Serial.printf(">> %s\n", cmd.c_str());

  if (u == "ATE0")                 { echoOn = false; ok(); }
  else if (u == "ATE1" || u == "ATE") { echoOn = true; ok(); }
  else if (u == "ATI" || u.startsWith("AT+CGMI") || u.startsWith("AT+GMI")) { sendRaw("HUAWEI"); ok(); }
  else if (u.startsWith("AT+CGMM") || u.startsWith("AT+GMM")) { sendRaw("E1550"); ok(); }
  else if (u.startsWith("AT+CGMR") || u.startsWith("AT+GMR")) { sendRaw("11.608.12.02.00"); ok(); }
  else if (u.startsWith("AT+CGSN") || u.startsWith("AT+GSN") || u.startsWith("AT+CIMI")) { sendRaw("123456789012345"); ok(); }
  else if (u.startsWith("AT+CPIN?"))  { sendRaw("+CPIN: READY"); ok(); }
  else if (u.startsWith("AT+CSQ"))    { sendRaw("+CSQ: 25,99"); ok(); }
  else if (u.startsWith("AT+CREG?"))  { sendRaw("+CREG: 0,1"); ok(); }
  else if (u.startsWith("AT+CGREG?")) { sendRaw("+CGREG: 0,1"); ok(); }
  else if (u.startsWith("AT+CGATT?")) { sendRaw("+CGATT: 1"); ok(); }
  else if (u.startsWith("AT+COPS?"))  { sendRaw("+COPS: 0,0,\"ESP32 NET\",2"); ok(); }
  else if (u.startsWith("ATD")) {
    // الرسيفر يتصل (غالباً ATD*99#) -> ندخلوا وضع البيانات
    sendRaw("CONNECT 7200000");
    dataMode = true;
    dataBytes = 0;
    lastDataMs = millis();
    Serial.println("==== DATA MODE: waiting for PPP frames (7E ...) ====");
  }
  else {
    // أي أمر ثاني (ATZ, AT+CGDCONT, AT^..., AT+CFUN...) نردوا OK
    ok();
  }
}

// ---------- أحداث الـ USB ----------
static void usbEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == ARDUINO_USB_EVENTS) {
    switch (id) {
      case ARDUINO_USB_STARTED_EVENT: Serial.println("[USB] started (host enumerated us)"); break;
      case ARDUINO_USB_STOPPED_EVENT: Serial.println("[USB] stopped"); break;
      case ARDUINO_USB_SUSPEND_EVENT: Serial.println("[USB] suspend"); break;
      case ARDUINO_USB_RESUME_EVENT:  Serial.println("[USB] resume"); break;
    }
  } else if (base == ARDUINO_USB_CDC_EVENTS) {
    arduino_usb_cdc_event_data_t *d = (arduino_usb_cdc_event_data_t *)data;
    switch (id) {
      case ARDUINO_USB_CDC_CONNECTED_EVENT:    Serial.println("[CDC] port opened by receiver"); break;
      case ARDUINO_USB_CDC_DISCONNECTED_EVENT: Serial.println("[CDC] port closed"); break;
      case ARDUINO_USB_CDC_LINE_STATE_EVENT:
        Serial.printf("[CDC] DTR=%u RTS=%u\n", d->line_state.dtr, d->line_state.rts);
        if (!d->line_state.dtr && dataMode) {
          dataMode = false;
          Serial.println("==== DTR dropped -> back to AT mode ====");
        }
        break;
      case ARDUINO_USB_CDC_LINE_CODING_EVENT:
        Serial.printf("[CDC] baud=%lu bits=%u parity=%u stop=%u\n",
                      (unsigned long)d->line_coding.bit_rate, d->line_coding.data_bits,
                      d->line_coding.parity, d->line_coding.stop_bits);
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== ESP32-S3 Fake 3G Modem - Stage 1 (sniffer) ===");

#if PROFILE == 1
  USB.VID(0x12D1); USB.PID(0x1001);
  USB.manufacturerName("HUAWEI"); USB.productName("HUAWEI Mobile");
#elif PROFILE == 2
  USB.VID(0x12D1); USB.PID(0x1506);
  USB.manufacturerName("HUAWEI"); USB.productName("HUAWEI Mobile");
#endif
  Serial.printf("Profile = %d\n", PROFILE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("[WiFi] connecting...");

  USB.onEvent(usbEvent);
  modem.onEvent(usbEvent);
  modem.begin();
  USB.begin();
  Serial.println("Plug the native USB port into the receiver, then open 3G menu.");
}

void loop() {
  // تقرير الوايفاي مرة وحدة لما يتصل
  if (!wifiReported && WiFi.status() == WL_CONNECTED) {
    wifiReported = true;
    Serial.printf("[WiFi] connected! IP=%s  RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }

  if (!dataMode) {
    // ===== وضع الأوامر AT =====
    while (modem.available()) {
      char c = modem.read();
      if (echoOn) modem.write(c);
      if (c == '\r') { handleAT(lineBuf); lineBuf = ""; }
      else if (c != '\n' && lineBuf.length() < 200) lineBuf += c;
    }
  } else {
    // ===== وضع البيانات: نطبعوا كل شي بالـ HEX =====
    uint8_t buf[16];
    while (modem.available()) {
      int n = modem.read(buf, sizeof(buf));
      if (n <= 0) break;
      Serial.printf("%06lu: ", (unsigned long)dataBytes);
      for (int i = 0; i < n; i++) Serial.printf("%02X ", buf[i]);
      Serial.println();
      dataBytes += n;
      lastDataMs = millis();
    }
    if (millis() - lastDataMs > 30000) {
      Serial.printf("[info] no data for 30s (total %lu bytes)\n", (unsigned long)dataBytes);
      lastDataMs = millis();
    }
  }
  delay(1);
}
