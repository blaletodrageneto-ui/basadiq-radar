/*
 * Basadiq Radar - WiFi CSI motion sensor with setup portal + live web app
 * Author : Ibrahim Adel Basadiq
 * Board  : ESP32, Arduino core 2.0.17 recommended. No extra libraries.
 *
 * First boot : ESP32 creates WiFi "Basadiq-Setup" -> connect with phone -> a page opens
 *              -> choose your WiFi + password -> saved -> reboots and connects.
 * Next boots : opens live radar page at the ESP32's IP (see Serial) or http://basadiq.local
 */
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "esp_wifi.h"
#if __has_include("ping/ping_sock.h")
#include "ping/ping_sock.h"
#define HAVE_PING 1
#endif
#include <math.h>

const char* AP_NAME = "Basadiq-Setup";   // open network, no password
const int   CAL_SECONDS = 15;
const float ALPHA = 0.15;

WebServer server(80);
DNSServer dns;
Preferences prefs;
String optionsHtml;
bool apMode = false;

#define MAX_SC 64
volatile float motion = 0, baseline = 0;
volatile bool calibrated = false;
volatile uint32_t pkts = 0;
float prevAmp[MAX_SC]; float scM[MAX_SC] = {0}; bool havePrev = false;
float calSum = 0; uint32_t calN = 0, bootMs = 0;
bool csiOk = false, modeDecided = false, useRssi = false;
float minStep = 0.01f;

// ---------------- motion engine ----------------
void updateMotion(float d) {
  motion = (1 - ALPHA) * motion + ALPHA * d;
  if (!calibrated) {
    calSum += motion; calN++;
    if (millis() - bootMs > CAL_SECONDS * 1000UL && calN > 30) { baseline = calSum / calN; calibrated = true; }
  }
}

void csiCallback(void* ctx, wifi_csi_info_t* info) {
  if (!info || !info->buf) return;
  pkts++;
  if (useRssi) return;
  int n = info->len / 2; if (n > MAX_SC) n = MAX_SC;
  float amp[MAX_SC], sum = 0;
  for (int i = 0; i < n; i++) {
    float im = info->buf[2 * i], re = info->buf[2 * i + 1];
    amp[i] = sqrtf(re * re + im * im); sum += amp[i];
  }
  float mean = sum / n; if (mean < 1) return;
  for (int i = 0; i < n; i++) amp[i] /= mean;
  if (havePrev) {
    float d = 0;
    for (int i = 0; i < n; i++) { float x = fabsf(amp[i] - prevAmp[i]); d += x; scM[i] = 0.85f * scM[i] + 0.15f * x; }
    updateMotion(d / n);
  }
  for (int i = 0; i < n; i++) prevAmp[i] = amp[i];
  havePrev = true;
}

void startCSI() {
  wifi_csi_config_t cfg = {};
  cfg.lltf_en = true; cfg.ltf_merge_en = true; cfg.channel_filter_en = false;
  esp_err_t a = esp_wifi_set_csi_config(&cfg);
  esp_err_t b = esp_wifi_set_csi_rx_cb(&csiCallback, NULL);
  esp_err_t c = esp_wifi_set_csi(true);
  csiOk = (a == ESP_OK && b == ESP_OK && c == ESP_OK);
  Serial.printf("INFO,CSI setup %d %d %d -> %s\n", a, b, c, csiOk ? "enabled" : "NOT supported, RSSI fallback");
}

void startPing() {
#ifdef HAVE_PING
  IPAddress g = WiFi.gatewayIP();
  ip_addr_t target; IP_ADDR4(&target, g[0], g[1], g[2], g[3]);
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = target; cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = 20; cfg.timeout_ms = 500;
  esp_ping_callbacks_t cbs = {}; esp_ping_handle_t h;
  if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK && esp_ping_start_session(h) == ESP_OK)
    Serial.println("INFO,ping started (20 ms)");
  else Serial.println("INFO,ping failed to start");
#else
  Serial.println("INFO,ping lib not in this core -> using TCP traffic fallback");
#endif
}

// ---------------- pages ----------------
const char SETUP_HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="ar" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Basadiq Setup</title>
<style>body{font-family:system-ui,Tahoma;background:#0b1118;color:#e6f2ea;padding:20px;max-width:480px;margin:auto}
h2{margin:0}small{color:#8aa}input,select,button{width:100%;padding:12px;margin:8px 0;border-radius:10px;border:0;font-size:16px;box-sizing:border-box}
input,select{background:#18222d;color:#fff}button{background:#2dff8a;color:#03140a;font-weight:700}</style></head><body>
<h2>إعداد Basadiq Radar</h2><small>Ibrahim Adel Basadiq</small>
<p>اختر شبكة الواي فاي حقتك (2.4GHz) واكتب كلمة السر:</p>
<form method="POST" action="/save"><select name="s"><option value="">-- اختر الشبكة --</option>)HTML";
const char SETUP_TAIL[] PROGMEM = R"HTML(</select>
<input name="t" placeholder="أو اكتب اسم الشبكة يدوياً">
<input name="p" type="password" placeholder="كلمة السر">
<button>حفظ واتصال</button></form>
<a href="/rescan" style="color:#8fd3ff">إعادة البحث عن الشبكات</a></body></html>)HTML";

const char LIVE[] PROGMEM = R"HTML(<!doctype html><html lang="ar" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Basadiq Radar</title>
<style>
body{margin:0;background:#070b10;color:#dff3e6;font-family:system-ui,Tahoma;padding:14px}
.w{max-width:800px;margin:auto}h1{font-size:20px;margin:0}.by{color:#7f97a0;font-size:12px;margin-bottom:10px}
#st{margin:10px 0;text-align:center;font-size:28px;font-weight:800;padding:16px;border-radius:14px;background:#26343f}
#st.on{background:#ff4a4a}#st.e{background:#14803f}
button{background:#2dff8a;color:#03140a;border:0;padding:9px 14px;border-radius:10px;font-weight:700;font-size:14px}
.g{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}@media(max-width:600px){.g{grid-template-columns:1fr}}
canvas{width:100%;background:#04080c;border-radius:12px;display:block}
.r{margin:8px 0;font-size:14px}.r input{width:60%}small,a{color:#7f97a0;font-size:12px}
</style></head><body><div class="w">
<h1>Basadiq Radar</h1><div class="by">Ibrahim Adel Basadiq</div>
<button id="cal">معايرة (المكان ساكن ٨ ثواني)</button>
<div id="st">...</div>
<div class="r">الحساسية: <b id="fv">1.8</b> <input id="f" type="range" min="1.1" max="5" step="0.1" value="1.8">
<br>حركة: <b id="m">0</b> | سكون: <b id="b">0</b> | حزم/ث: <b id="r">0</b> | الوضع: <b id="mode">-</b></div>
<div class="g"><canvas id="rad" width="400" height="400"></canvas><canvas id="hm" width="400" height="400"></canvas></div>
<p><small>يكشف الحركة فقط (مو صورة ولا مكان). استخدمه في مكانك أو بموافقة أصحاب المكان.</small><br><a href="/reset">تغيير شبكة الواي فاي</a></p>
</div><script>
const $=i=>document.getElementById(i),R=$('rad').getContext('2d'),H=$('hm').getContext('2d');
let base=0,factor=1.8,calUntil=0,cs=0,cn=0,sm=0,ang=0,lp=0,lt=0,ratio=0;const rows=[];
$('f').oninput=e=>{factor=+e.target.value;$('fv').textContent=factor.toFixed(1)};
$('cal').onclick=()=>{calUntil=Date.now()+8000;cs=0;cn=0};
const S=(t,c)=>{$('st').textContent=t;$('st').className=c||''};
const heat=v=>{const t=Math.min(1,v/250);return `hsl(${240-240*t},90%,${12+45*t}%)`};
function dh(){H.clearRect(0,0,400,400);const cw=400/32,ch=400/60;rows.forEach((r,y)=>r.forEach((v,x)=>{H.fillStyle=heat(v);H.fillRect(x*cw,y*ch,cw+1,ch+1)}))}
function dr(){const c=200;R.clearRect(0,0,400,400);sm+=(ratio-sm)*.15;const lv=Math.max(0,Math.min(1,(sm-.7)/1));
R.strokeStyle='#1c3b2a';for(let i=1;i<=4;i++){R.beginPath();R.arc(c,c,i*48,0,7);R.stroke()}
const col=lv>.05?'255,74,74':'45,255,138',g=R.createRadialGradient(c,c,0,c,c,190);
g.addColorStop(0,`rgba(${col},${.15+.6*lv})`);g.addColorStop(1,`rgba(${col},0)`);R.fillStyle=g;R.beginPath();R.arc(c,c,190,0,7);R.fill();
ang+=.04;R.save();R.translate(c,c);R.rotate(ang);const sw=R.createLinearGradient(0,0,190,0);sw.addColorStop(0,'rgba(45,255,138,.45)');sw.addColorStop(1,'rgba(45,255,138,0)');
R.fillStyle=sw;R.beginPath();R.moveTo(0,0);R.arc(0,0,190,-.35,0);R.fill();R.restore();
R.strokeStyle=`rgba(${col},.9)`;R.lineWidth=2;R.beginPath();R.arc(c,c,20+lv*160+((Date.now()/20)%20)*lv,0,7);R.stroke();requestAnimationFrame(dr)}
async function tick(){try{const j=await(await fetch('/data')).json();const n=Date.now(),m=j.m;
$('m').textContent=m.toFixed(3);$('mode').textContent=j.mode;
if(n-lt>1000){$('r').textContent=Math.round((j.k-lp)*1000/(n-lt));lp=j.k;lt=n}
if(n<calUntil){cs+=m;cn++;base=cs/cn;S('جاري المعايرة... لا تتحرك','');ratio=0}
else{if(!calUntil&&j.cal)base=j.b;const thr=Math.max(base*factor,base+j.ms);
if(!base){S('جاري المعايرة...','');ratio=0}else{ratio=m/thr;S(m>thr?'حركة مكتشفة':'لا توجد حركة',m>thr?'on':'e')}}
$('b').textContent=base.toFixed(3);rows.push(j.sc);if(rows.length>60)rows.shift();dh()}catch(e){S('انقطع الاتصال','')}
setTimeout(tick,250)}
tick();dr();
</script></body></html>)HTML";

// ---------------- portal ----------------
void scanNets() {
  int n = WiFi.scanNetworks();
  optionsHtml = "";
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i); if (!s.length()) continue;
    s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace("\"", "&quot;");
    optionsHtml += "<option>" + s + "</option>";
  }
}

void startPortal() {
  apMode = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME);
  delay(200);
  scanNets();
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", String(SETUP_HEAD) + optionsHtml + String(SETUP_TAIL)); });
  server.on("/rescan", HTTP_GET, []() { scanNets(); server.sendHeader("Location", "/"); server.send(302); });
  server.on("/save", HTTP_POST, []() {
    String s = server.arg("t"); if (!s.length()) s = server.arg("s");
    if (!s.length()) { server.send(400, "text/plain; charset=utf-8", "اختر شبكة"); return; }
    prefs.putString("ssid", s); prefs.putString("pass", server.arg("p"));
    server.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><body style='font-family:sans-serif;text-align:center;padding:40px'><h2>تم الحفظ</h2><p>اللوحة تعيد التشغيل وتتصل بـ " + s + "<br>ارجع لشبكتك العادية وافتح <b>http://basadiq.local</b> أو شوف الـ IP في Serial Monitor.</p>");
    delay(1500); ESP.restart();
  });
  server.onNotFound([]() { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302); });
  server.begin();
  Serial.println("INFO,SETUP MODE: connect to WiFi 'Basadiq-Setup' and open http://192.168.4.1");
}

void startLive() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", LIVE); });
  server.on("/data", HTTP_GET, []() {
    String j = "{\"m\":" + String(motion, 4) + ",\"b\":" + String(baseline, 4) + ",\"cal\":" + (calibrated ? "true" : "false") +
               ",\"k\":" + String(pkts) + ",\"ms\":" + String(minStep, 3) + ",\"mode\":\"" + (useRssi ? "RSSI" : "CSI") + "\",\"sc\":[";
    for (int g = 0; g < 32; g++) { j += String((int)((scM[2*g] + scM[2*g+1]) * 500)); if (g < 31) j += ","; }
    j += "]}";
    server.send(200, "application/json", j);
  });
  server.on("/reset", HTTP_GET, []() {
    prefs.clear(); server.send(200, "text/html; charset=utf-8", "<meta charset=utf-8><h3>تم المسح، اللوحة تعيد التشغيل...</h3>");
    delay(1000); ESP.restart();
  });
  server.begin();
  MDNS.begin("basadiq");
}

void setup() {
  Serial.begin(115200); delay(300);
  prefs.begin("wifi", false);
  String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
  if (!ssid.length()) { startPortal(); return; }

  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("INFO,connecting to %s\n", ssid.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(300);
  if (WiFi.status() != WL_CONNECTED) { Serial.println("INFO,connect failed -> setup mode"); startPortal(); return; }

  Serial.printf("INFO,connected. OPEN http://%s  (or http://basadiq.local)  RSSI=%d CH=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
  bootMs = millis();
  startCSI(); startPing(); startLive();
}

void loop() {
  server.handleClient();
  if (apMode) { dns.processNextRequest(); delay(2); return; }

  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(1000); return; }

  if (!modeDecided && millis() - bootMs > 6000) {
    modeDecided = true; useRssi = (pkts < 20);
    if (useRssi) { minStep = 0.3f; Serial.println("INFO,no CSI packets -> RSSI fallback"); }
    else Serial.printf("INFO,CSI mode OK (%lu packets)\n", (unsigned long)pkts);
  }
#ifndef HAVE_PING
  { static uint32_t lt = 0;
    if (millis() - lt > 25) { lt = millis(); WiFiClient c; c.connect(WiFi.gatewayIP(), 80, 15); c.stop(); } }
#endif
  static uint32_t lr = 0; static float mean = WiFi.RSSI();
  if (useRssi && millis() - lr > 20) {
    lr = millis(); float r = WiFi.RSSI(); mean += 0.1f * (r - mean);
    updateMotion(fabsf(r - mean)); pkts++;
  }
  delay(2);
}
