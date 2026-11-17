/*
  ZatzKey — الـ ESP32-S3 يولّي كيبورد + ماوس USB، وتتحكم فيه من التلفون
  ----------------------------------------------------------------------
  - تركّب منفذ الـ USB الأصلي (OTG) في اللابتوب -> يشوفه كيبورد وماوس حقيقيين
    (يخدم في كالي، والويندوز، وحتى الـ BIOS وشاشات التنصيب)
  - تفتح صفحة الويب من التلفون وتكتب وتحرك الماوس

  طريقتين للدخول من التلفون:
    1) شبكة ZatzKey مباشرة (دايماً تخدم):  وايفاي ZatzKey / كلمة السر zatz1234
       وبعدين افتح:  http://192.168.4.1
    2) وايفاي الدار: الـ IP يطلع في الـ Serial Monitor

  إعدادات Arduino IDE (Tools):
    Board            : ESP32S3 Dev Module
    USB Mode         : USB-OTG (TinyUSB)
    USB CDC On Boot  : Disabled
    Flash Size       : 16MB
    PSRAM            : OPI PSRAM
  ارفع من منفذ الـ COM، وبعدين ركّب منفذ الـ USB الأصلي في لابتوب كالي.
*/

#include <WiFi.h>
#include <WebServer.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

// ===== وايفاي الدار =====
const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ===== الشبكة الخاصة بالكيبورد (احتياطية، دايماً شغالة) =====
const char *AP_SSID = "ZatzKey";
const char *AP_PASS = "zatz1234";   // 8 حروف على الأقل

USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
WebServer server(80);
bool staReported = false;

// ---------- أسماء الأزرار الخاصة ----------
struct KeyName { const char *n; uint8_t k; };
const KeyName KEYS[] = {
  {"enter", KEY_RETURN}, {"bksp", KEY_BACKSPACE}, {"tab", KEY_TAB}, {"esc", KEY_ESC},
  {"up", KEY_UP_ARROW}, {"down", KEY_DOWN_ARROW}, {"left", KEY_LEFT_ARROW}, {"right", KEY_RIGHT_ARROW},
  {"del", KEY_DELETE}, {"home", KEY_HOME}, {"end", KEY_END},
  {"pgup", KEY_PAGE_UP}, {"pgdn", KEY_PAGE_DOWN}, {"space", ' '},
  {"ctrl", KEY_LEFT_CTRL}, {"alt", KEY_LEFT_ALT}, {"shift", KEY_LEFT_SHIFT}, {"super", KEY_LEFT_GUI},
  {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4}, {"f5", KEY_F5}, {"f6", KEY_F6},
  {"f7", KEY_F7}, {"f8", KEY_F8}, {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
};

int findKey(const String &name) {
  for (auto &k : KEYS) if (name.equalsIgnoreCase(k.n)) return k.k;
  if (name.length() == 1) return (uint8_t)name[0];   // حرف عادي زي c أو v
  return -1;
}

// ---------- صفحة التلفون ----------
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="ar" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>ZatzKey</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:10px;background:#0f1720;color:#d6e2ee;font-family:sans-serif}
h1{font-size:18px;margin:4px 0 10px;color:#5ad1e6}
.sec{margin-bottom:12px}
input,textarea{width:100%;padding:12px;font-size:17px;border-radius:10px;border:1px solid #2b3a4a;background:#17222e;color:#fff;direction:ltr}
.row{display:flex;flex-wrap:wrap;gap:6px;margin-top:6px}
button{flex:1 0 22%;padding:12px 4px;font-size:15px;border:0;border-radius:10px;background:#22313f;color:#fff}
button:active{background:#5ad1e6;color:#000}
.wide{flex:1 0 46%}
#pad{height:220px;border-radius:14px;background:#17222e;border:1px dashed #3a5068;display:flex;align-items:center;justify-content:center;color:#5b7088;touch-action:none}
small{color:#7b8fa3}
</style></head><body>
<h1>⌨️ ZatzKey</h1>

<div class="sec">
<small>كتابة مباشرة (كل حرف يتبعت على طول):</small>
<input id="live" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false" placeholder="اكتب هنا...">
</div>

<div class="sec">
<small>أو اكتب نص كامل وابعته مرة وحدة:</small>
<textarea id="box" rows="3" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false"></textarea>
<div class="row"><button class="wide" onclick="sendBox(false)">ابعت</button><button class="wide" onclick="sendBox(true)">ابعت + Enter</button></div>
</div>

<div class="sec"><div class="row">
<button onclick="k('esc')">Esc</button><button onclick="k('tab')">Tab</button><button onclick="k('bksp')">⌫</button><button onclick="k('enter')">Enter</button>
<button onclick="k('home')">Home</button><button onclick="k('up')">▲</button><button onclick="k('end')">End</button><button onclick="k('del')">Del</button>
<button onclick="k('left')">◀</button><button onclick="k('down')">▼</button><button onclick="k('right')">▶</button><button onclick="k('space')">Space</button>
</div></div>

<div class="sec"><small>اختصارات:</small><div class="row">
<button onclick="c('ctrl+alt+t')">تيرمينال</button><button onclick="c('ctrl+c')">Ctrl+C</button><button onclick="c('ctrl+shift+c')">نسخ تيرمينال</button><button onclick="c('ctrl+shift+v')">لصق تيرمينال</button>
<button onclick="c('ctrl+v')">Ctrl+V</button><button onclick="c('ctrl+z')">Ctrl+Z</button><button onclick="c('ctrl+a')">Ctrl+A</button><button onclick="c('ctrl+l')">Ctrl+L</button>
<button onclick="c('alt+tab')">Alt+Tab</button><button onclick="c('alt+f4')">Alt+F4</button><button onclick="k('super')">Super</button><button onclick="c('ctrl+alt+del')">Ctrl+Alt+Del</button>
</div></div>

<div class="sec"><small>ماوس (حرّك صبعك، ونقرة = كليك):</small>
<div id="pad">لوحة الماوس</div>
<div class="row"><button class="wide" onclick="go('/click?b=l')">كليك يسار</button><button class="wide" onclick="go('/click?b=r')">كليك يمين</button>
<button class="wide" onclick="go('/scroll?d=3')">سكرول ▲</button><button class="wide" onclick="go('/scroll?d=-3')">سكرول ▼</button></div>
</div>

<script>
let q=Promise.resolve();
function go(u){q=q.then(()=>fetch(u).catch(()=>{}));}
function t(s){go('/type?t='+encodeURIComponent(s));}
function k(n){go('/key?k='+n);}
function c(x){go('/combo?c='+encodeURIComponent(x));}
function sendBox(enter){const b=document.getElementById('box');if(b.value)t(b.value);if(enter)k('enter');b.value='';}

const live=document.getElementById('live');
live.addEventListener('input',e=>{
  if(e.inputType==='deleteContentBackward'){k('bksp');}
  else if(e.data){t(e.data);}
  live.value='';
});
live.addEventListener('keydown',e=>{if(e.key==='Enter'){k('enter');e.preventDefault();}});

const pad=document.getElementById('pad');const SENS=1.6;
let lx=0,ly=0,ax=0,ay=0,moved=false;
pad.addEventListener('touchstart',e=>{const p=e.touches[0];lx=p.clientX;ly=p.clientY;moved=false;e.preventDefault();},{passive:false});
pad.addEventListener('touchmove',e=>{const p=e.touches[0];ax+=(p.clientX-lx)*SENS;ay+=(p.clientY-ly)*SENS;lx=p.clientX;ly=p.clientY;moved=true;e.preventDefault();},{passive:false});
pad.addEventListener('touchend',e=>{if(!moved)go('/click?b=l');e.preventDefault();},{passive:false});
setInterval(()=>{const dx=Math.round(ax),dy=Math.round(ay);if(dx||dy){ax-=dx;ay-=dy;go('/mouse?dx='+dx+'&dy='+dy);}},40);
</script></body></html>
)HTML";

// ---------- معالجات الطلبات ----------
void hRoot() { server.send_P(200, "text/html; charset=utf-8", PAGE); }

void hType() {
  String s = server.arg("t");
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch == '\n') Keyboard.write(KEY_RETURN);
    else if (ch >= 32 && ch < 127) Keyboard.write(ch);   // الكيبورد USB يكتب حروف إنجليزية بس
  }
  server.send(200, "text/plain", "ok");
}

void hKey() {
  int code = findKey(server.arg("k"));
  if (code >= 0) Keyboard.write((uint8_t)code);
  server.send(200, "text/plain", code >= 0 ? "ok" : "unknown");
}

void hCombo() {
  String c = server.arg("c") + "+";
  int start = 0;
  for (int i = 0; i < (int)c.length(); i++) {
    if (c[i] == '+') {
      int code = findKey(c.substring(start, i));
      if (code >= 0) Keyboard.press((uint8_t)code);
      start = i + 1;
    }
  }
  delay(60);
  Keyboard.releaseAll();
  server.send(200, "text/plain", "ok");
}

void hMouse() {
  int dx = constrain(server.arg("dx").toInt(), -127, 127);
  int dy = constrain(server.arg("dy").toInt(), -127, 127);
  Mouse.move(dx, dy);
  server.send(200, "text/plain", "ok");
}

void hClick() {
  Mouse.click(server.arg("b") == "r" ? MOUSE_RIGHT : MOUSE_LEFT);
  server.send(200, "text/plain", "ok");
}

void hScroll() {
  Mouse.move(0, 0, constrain(server.arg("d").toInt(), -10, 10));
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ZatzKey ===");

  // الكيبورد والماوس USB
  Keyboard.begin();
  Mouse.begin();
  USB.productName("ZatzKey");
  USB.manufacturerName("Zatz");
  USB.begin();

  // الوايفاي: شبكة خاصة + وايفاي الدار مع بعض
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[AP] WiFi '%s' pass '%s' -> http://%s\n", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("[WiFi] connecting to home WiFi...");

  server.on("/", hRoot);
  server.on("/type", hType);
  server.on("/key", hKey);
  server.on("/combo", hCombo);
  server.on("/mouse", hMouse);
  server.on("/click", hClick);
  server.on("/scroll", hScroll);
  server.begin();
}

void loop() {
  server.handleClient();
  if (!staReported && WiFi.status() == WL_CONNECTED) {
    staReported = true;
    Serial.printf("[WiFi] home WiFi OK -> http://%s\n", WiFi.localIP().toString().c_str());
  }
}
