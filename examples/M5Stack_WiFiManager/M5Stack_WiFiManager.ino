/*
 *  NTRIP client for M5Stack (M5Unified + WiFiManager)
 *  - Connects to NTRIP caster and forwards RTCM data to Serial2
 *  - LCD status display with byte counter
 *  - Hold BtnA at boot to enter WiFi config portal
 *  - After WiFi connect, open http://ntrip-client.local/ to edit NTRIP host/port/mountpoint/user/passwd
 *    (settings persist in NVS, applied immediately by reconnecting)
 *  - Firmware update check: on boot, queries GitHub Releases. New version is shown in yellow
 *    on the LCD header and as a banner on the web UI
 */
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include "NTRIPClient.h"

NTRIPClient    ntrip_c;
AsyncWebServer web(80);
Preferences    prefs;

// Forward declarations (required by PlatformIO's stricter auto-prototyper)
static const char* statusString(bool stalled);
void refreshUpdatingPage();

static const char* MDNS_NAME      = "ntrip-client";
static const char* FW_VERSION     = "0.6.0";
static const char* FW_REPO        = "yasunorioi/NTRIP-client-for-Arduino";
static const char* FW_BIN_NAME    = "m5stack-wifimanager.bin";

// ---- NTRIP Server Config (defaults; overridden by NVS) ----
char host[64]   = "rtk.toiso.fit";
int  httpPort   = 2101;
char mntpnt[64] = "eniwa-bd982";
char user[64]   = "";
char passwd[64] = "";

// ---- State ----
uint64_t totalBytes = 0;
uint64_t lastBytes  = 0;
unsigned long lastDataTime = 0;
const unsigned long STALL_TIMEOUT_MS = 5000;
int uart_bps = 115200;

enum NtripState { NS_IDLE, NS_OK, NS_CONFIG_ERR };
NtripState ntripState = NS_IDLE;
bool reconnectPending = false;

String latestVersion;
String releaseUrl;
bool   updateAvailable = false;

// ---- OTA state ----
enum OtaState { OTA_IDLE, OTA_PENDING, OTA_RUNNING, OTA_DONE, OTA_FAILED };
volatile OtaState otaState = OTA_IDLE;
int     otaProgress = 0;       // 0..100
String  otaError;

// ---- Tractor color presets (RGB565) ----
struct TractorPreset {
  const char* name;
  uint16_t    body;
  uint16_t    roof;
  uint16_t    hub;
};
static const TractorPreset TRACTOR_PRESETS[] = {
  { "John Deere (green)",   0x07E0, 0x03E0, 0xFFE0 },
  { "Kubota (orange)",      0xFB20, 0x8200, 0xC618 },
  { "Case IH (red)",        0xC800, 0x6800, 0xC618 },
  { "New Holland (blue)",   0x2C99, 0x1191, 0xFFE0 },
  { "White (vintage)",      0xFFFF, 0xC618, 0x4208 },
  { "Claas (RAL 6010)",     0x4C88, 0xFFFF, 0xC000 },
  { "Yanmar (red/black)",   0xC125, 0x2104, 0xC618 },
  { "Iseki (blue)",         0x033F, 0xFFFF, 0xC618 },
  { "Deutz-Fahr (green)",   0x4DE0, 0x2104, 0xC618 },
};
static const int NUM_TRACTOR_PRESETS = sizeof(TRACTOR_PRESETS) / sizeof(TRACTOR_PRESETS[0]);
int currentTractorIdx = 0;

// ---- Tractor animation ----
static const int TRACTOR_W = 32;
static const int TRACTOR_H = 26;
static const int TRACTOR_DISP_Y = 190;
static const int TRACTOR_SPEED = 4;
static const unsigned long TRACTOR_INTERVAL_MS = 100;
M5Canvas tractorCanvas(&M5.Display);
int           tractorX = -TRACTOR_W;
unsigned long lastTractorTick = 0;

// ---- Display page state ----
enum DisplayPage { PAGE_INFO, PAGE_SKY, PAGE_GRAPH, PAGE_QR, PAGE_UPDATE_CONFIRM, PAGE_UPDATING };
DisplayPage currentPage   = PAGE_INFO;
bool        displayAsleep = false;

// ---- NMEA / satellite tracking (from Serial2 RX) ----
struct SatInfo {
  uint8_t  cons;       // 0=GPS, 1=GLO, 2=GAL, 3=BDS, 4=QZS
  uint8_t  prn;
  uint8_t  elev;       // 0..90 deg
  uint16_t az;         // 0..359 deg
  uint8_t  snr;        // dBHz (0 = not tracked)
  unsigned long lastSeen;
};
static const int MAX_SATS = 64;
SatInfo sats[MAX_SATS];
int     satCount = 0;
unsigned long lastNmeaTime = 0;
char    nmeaBuf[128];
int     nmeaIdx = 0;

// ---- 5-min data rate buckets (1h history = 12 buckets) ----
static const int           BUCKET_COUNT = 12;
static const unsigned long BUCKET_MS    = 5UL * 60UL * 1000UL;
uint32_t      rxBuckets[BUCKET_COUNT] = {0};
int           bucketHead   = 0;        // index of current (newest) bucket
unsigned long bucketStartMs = 0;
uint64_t      bucketStartBytes = 0;

// ---------- NVS ----------
void loadSettings() {
  prefs.begin("ntrip", true);
  String h = prefs.getString("host",   host);
  int    p = prefs.getInt   ("port",   httpPort);
  String m = prefs.getString("mntpnt", mntpnt);
  String u = prefs.getString("user",   user);
  String w = prefs.getString("passwd", passwd);
  int    t = prefs.getInt   ("tractor", 0);
  prefs.end();
  strlcpy(host,   h.c_str(), sizeof(host));
  httpPort = p;
  strlcpy(mntpnt, m.c_str(), sizeof(mntpnt));
  strlcpy(user,   u.c_str(), sizeof(user));
  strlcpy(passwd, w.c_str(), sizeof(passwd));
  if (t < 0 || t >= NUM_TRACTOR_PRESETS) t = 0;
  currentTractorIdx = t;
}

void saveSettings() {
  prefs.begin("ntrip", false);
  prefs.putString("host",    host);
  prefs.putInt   ("port",    httpPort);
  prefs.putString("mntpnt",  mntpnt);
  prefs.putString("user",    user);
  prefs.putString("passwd",  passwd);
  prefs.putInt   ("tractor", currentTractorIdx);
  prefs.end();
}

// ---------- WiFi ----------
void setupWiFi() {
  WiFiManager wm;

  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);
  M5.Display.println("WiFi setup");
  M5.Display.println("Hold BtnA for config");

  bool doManualConfig = false;
  for (int i = 0; i < 200; i++) {
    M5.update();
    if (M5.BtnA.isPressed()) {
      doManualConfig = true;
      break;
    }
    delay(10);
  }

  if (doManualConfig) {
    M5.Display.println("Config portal active");
    M5.Display.println("SSID: NTRIP-Client");
    wm.startConfigPortal("NTRIP-Client");
  } else {
    M5.Display.println("Connecting...");
    wm.autoConnect("NTRIP-Client");
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);   // モデムスリープOFF: NTRIP常時受信の取りこぼし/切断を抑制
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    M5.Display.print("IP: ");
    M5.Display.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed");
    M5.Display.println("WiFi FAILED");
    delay(3000);
    ESP.restart();
  }
}

// ---------- FW update check ----------
static bool semverNewer(const String &a, const String &b) {
  int ai[3] = {0,0,0}, bi[3] = {0,0,0};
  sscanf(a.c_str(), "%d.%d.%d", &ai[0], &ai[1], &ai[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bi[0], &bi[1], &bi[2]);
  for (int i = 0; i < 3; i++) {
    if (ai[i] > bi[i]) return true;
    if (ai[i] < bi[i]) return false;
  }
  return false;
}

static String extractJsonString(const String &body, const char* key) {
  String needle = String("\"") + key + "\":\"";
  int p = body.indexOf(needle);
  if (p < 0) return "";
  p += needle.length();
  int e = body.indexOf('"', p);
  if (e <= p) return "";
  return body.substring(p, e);
}

void checkLatestRelease() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  String url = String("https://api.github.com/repos/") + FW_REPO + "/releases/latest";
  if (!http.begin(client, url)) {
    Serial.println("FW check: http.begin failed");
    return;
  }
  http.addHeader("User-Agent", "NTRIP-Client-FW-Check");
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    String tag  = extractJsonString(body, "tag_name");
    String html = extractJsonString(body, "html_url");
    if (tag.length() > 0) {
      String ver = (tag.startsWith("v") || tag.startsWith("V")) ? tag.substring(1) : tag;
      latestVersion   = ver;
      releaseUrl      = html;
      updateAvailable = semverNewer(ver, FW_VERSION);
      Serial.printf("FW check: latest=%s current=%s update=%d\n",
                    ver.c_str(), FW_VERSION, updateAvailable ? 1 : 0);
    } else {
      Serial.println("FW check: tag_name not found");
    }
  } else {
    Serial.printf("FW check: HTTP %d\n", code);
  }
  http.end();
}

// ---------- NMEA parser (GSV only) ----------
static void addSatObservation(uint8_t cons, uint8_t prn, int elev, int az, int snr) {
  if (prn == 0) return;
  unsigned long now = millis();
  for (int i = 0; i < satCount; i++) {
    if (sats[i].cons == cons && sats[i].prn == prn) {
      if (elev >= 0) sats[i].elev = elev;
      if (az   >= 0) sats[i].az   = az;
      sats[i].snr      = (snr >= 0) ? snr : 0;
      sats[i].lastSeen = now;
      return;
    }
  }
  if (satCount < MAX_SATS) {
    sats[satCount].cons     = cons;
    sats[satCount].prn      = prn;
    sats[satCount].elev     = (elev >= 0) ? elev : 0;
    sats[satCount].az       = (az   >= 0) ? az   : 0;
    sats[satCount].snr      = (snr  >= 0) ? snr  : 0;
    sats[satCount].lastSeen = now;
    satCount++;
  }
}

static void pruneSats() {
  unsigned long now = millis();
  int j = 0;
  for (int i = 0; i < satCount; i++) {
    if (now - sats[i].lastSeen < 30000UL) {
      if (i != j) sats[j] = sats[i];
      j++;
    }
  }
  satCount = j;
}

// Parse numeric fields (positive ints or -1 for empty) from body until '*' or end.
static int parseIntFields(const char* body, int* out, int outSize) {
  int n = 0;
  const char* p = body;
  while (n < outSize && *p && *p != '*') {
    bool has = false;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); has = true; p++; }
    out[n++] = has ? v : -1;
    if (*p == ',') p++;
    else break;
  }
  return n;
}

static void parseGsvLine(const char* line) {
  // line starts at '$' and is null-terminated, no \r\n.
  // We only care about $G*GSV where * is P/L/A/B/Q.
  uint8_t cons;
  switch (line[2]) {
    case 'P': cons = 0; break;
    case 'L': cons = 1; break;
    case 'A': cons = 2; break;
    case 'B': cons = 3; break;
    case 'Q': cons = 4; break;
    default:  return;
  }
  const char* comma = strchr(line, ',');
  if (!comma) return;
  int fields[24];
  int n = parseIntFields(comma + 1, fields, 24);
  if (n < 3) return;
  // fields[0]=total_msgs, [1]=msg_num, [2]=total_sats
  // up to 4 sats follow: PRN, elev, az, snr  (n field positions 3..18)
  for (int i = 0; i < 4; i++) {
    int base = 3 + i * 4;
    if (base + 3 >= n) break;
    int prn  = fields[base];
    int elev = fields[base + 1];
    int az   = fields[base + 2];
    int snr  = fields[base + 3];
    if (prn > 0) addSatObservation(cons, (uint8_t)prn, elev, az, snr);
  }
}

static void parseNmeaLine(const char* line) {
  if (line[0] != '$') return;
  if (strlen(line) < 6) return;
  if (line[3] == 'G' && line[4] == 'S' && line[5] == 'V') {
    parseGsvLine(line);
    lastNmeaTime = millis();
  }
}

static void processNmeaChar(int c) {
  if (c == '$') {
    nmeaIdx = 0;
    nmeaBuf[nmeaIdx++] = '$';
  } else if (nmeaIdx > 0) {
    if (c == '\r' || c == '\n') {
      nmeaBuf[nmeaIdx] = 0;
      parseNmeaLine(nmeaBuf);
      nmeaIdx = 0;
    } else if (nmeaIdx < (int)sizeof(nmeaBuf) - 1) {
      nmeaBuf[nmeaIdx++] = (char)c;
    } else {
      nmeaIdx = 0;
    }
  }
}

// ---------- 5-min bucket update ----------
static void updateBuckets() {
  unsigned long now = millis();
  if (bucketStartMs == 0) {
    bucketStartMs    = now;
    bucketStartBytes = totalBytes;
  }
  rxBuckets[bucketHead] = (uint32_t)(totalBytes - bucketStartBytes);
  if (now - bucketStartMs >= BUCKET_MS) {
    bucketHead = (bucketHead + 1) % BUCKET_COUNT;
    rxBuckets[bucketHead] = 0;
    bucketStartMs    = now;
    bucketStartBytes = totalBytes;
  }
}

// ---------- OTA ----------
void runOta() {
  otaState    = OTA_RUNNING;
  otaProgress = 0;
  otaError    = "";

  String url = String("https://github.com/") + FW_REPO +
               "/releases/download/v" + latestVersion + "/" + FW_BIN_NAME;
  Serial.printf("OTA: GET %s\n", url.c_str());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    otaError = "http.begin failed";
    otaState = OTA_FAILED;
    return;
  }
  http.addHeader("User-Agent", "NTRIP-Client-OTA");
  int code = http.GET();
  if (code != 200) {
    otaError = "HTTP " + String(code);
    otaState = OTA_FAILED;
    http.end();
    return;
  }
  int total = http.getSize();
  if (total <= 0) {
    otaError = "no content-length";
    otaState = OTA_FAILED;
    http.end();
    return;
  }
  if (!Update.begin((size_t)total)) {
    otaError = String("Update.begin: ") + Update.errorString();
    otaState = OTA_FAILED;
    http.end();
    return;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
  int lastShown = -1;
  unsigned long lastYield = millis();
  while (http.connected() && written < total) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail > sizeof(buf) ? sizeof(buf) : avail;
      int r = stream->readBytes(buf, toRead);
      if (r <= 0) break;
      if (Update.write(buf, r) != (size_t)r) {
        otaError = String("write: ") + Update.errorString();
        Update.abort();
        otaState = OTA_FAILED;
        http.end();
        return;
      }
      written += r;
      otaProgress = (int)((int64_t)written * 100 / total);
      if (otaProgress != lastShown) {
        lastShown = otaProgress;
        refreshUpdatingPage();
      }
    } else {
      delay(1);
    }
    if (millis() - lastYield > 50) { yield(); lastYield = millis(); }
  }
  http.end();
  if (written != total) {
    otaError = "short read";
    Update.abort();
    otaState = OTA_FAILED;
    return;
  }
  if (!Update.end(true)) {
    otaError = String("Update.end: ") + Update.errorString();
    otaState = OTA_FAILED;
    return;
  }
  Serial.println("OTA: success, restarting");
  otaState = OTA_DONE;
  delay(500);
  ESP.restart();
}

// ---------- Web UI ----------
static String htmlEscape(const String &s) {
  String o; o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += "&amp;";  break;
      case '<': o += "&lt;";   break;
      case '>': o += "&gt;";   break;
      case '"': o += "&quot;"; break;
      default:  o += c;
    }
  }
  return o;
}

static String buildIndexHtml() {
  String body;
  body.reserve(3584);
  body += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>NTRIP Client</title>"
            "<style>body{font-family:sans-serif;max-width:480px;margin:1em auto;padding:0 1em}"
            "label{display:block;margin:.6em 0 .2em}input,select{width:100%;padding:.4em;font-size:1em}"
            ".row{display:flex;gap:.5em}.row>div{flex:1}"
            "button{margin-top:1em;padding:.6em 1.2em;font-size:1em}"
            ".s{margin-bottom:1em;padding:.5em;background:#eef;border-radius:4px}"
            ".ver{margin:.3em 0 1em;font-size:.9em;color:#555}"
            ".upd{display:inline-block;background:#fc6;color:#400;padding:.15em .6em;"
            "border-radius:3px;text-decoration:none;font-weight:bold;margin-left:.4em}"
            ".toast{position:fixed;top:1em;right:1em;background:#4a8;color:#fff;"
            "padding:.5em 1em;border-radius:3px;display:none;font-size:.9em}"
            ".toast.show{display:block}.toast.err{background:#c33}"
            "</style></head><body>");
  body += F("<h2>NTRIP Client</h2>");
  body += F("<div class='ver'>FW v");
  body += FW_VERSION;
  body += F("<span id='upd'></span></div>");
  body += F("<div id='ota' style='display:none;margin:.5em 0;padding:.5em;background:#fee;border-left:4px solid #c33;border-radius:3px'></div>");
  body += F("<div class='s'>State: <b id='state'>&mdash;</b><br>"
            "RX bytes: <span id='rx'>&mdash;</span><br>"
            "Uptime: <span id='up'>&mdash;</span> sec</div>");
  body += F("<form id='f' method='POST' action='/save'>");
  body += "<label>Host</label><input name='host' value='" + htmlEscape(host) + "'>";
  body += F("<div class='row'><div>");
  body += "<label>Port</label><input name='port' type='number' value='" + String(httpPort) + "'>";
  body += F("</div><div>");
  body += "<label>Mountpoint</label><input name='mntpnt' value='" + htmlEscape(mntpnt) + "'>";
  body += F("</div></div>");
  body += "<label>User</label><input name='user' value='" + htmlEscape(user) + "'>";
  body += "<label>Password</label><input name='passwd' type='password' value='" + htmlEscape(passwd) + "'>";
  body += F("<label>Tractor</label><select name='tractor'>");
  for (int i = 0; i < NUM_TRACTOR_PRESETS; i++) {
    body += "<option value='" + String(i) + "'";
    if (i == currentTractorIdx) body += " selected";
    body += ">" + String(TRACTOR_PRESETS[i].name) + "</option>";
  }
  body += F("</select>");
  body += F("<button id='b' type='submit'>Save &amp; Reconnect</button></form>");
  body += F("<div id='t' class='toast'></div>");
  body += F("<script>"
            "async function poll(){try{const r=await fetch('/status.json');const d=await r.json();"
            "document.getElementById('state').textContent=d.state;"
            "document.getElementById('rx').textContent=d.rx.toLocaleString();"
            "document.getElementById('up').textContent=d.uptime;"
            "const u=document.getElementById('upd');"
            "if(d.latest){u.innerHTML=` <button type='button' id='ub' style='background:#fc6;color:#400;"
            "border:none;padding:.2em .6em;border-radius:3px;font-weight:bold;cursor:pointer'>"
            "Update to v${d.latest}</button>`;"
            "document.getElementById('ub').onclick=async()=>{"
            "if(!confirm(`Update firmware to v${d.latest}?`))return;"
            "await fetch('/update',{method:'POST'});};}else{u.innerHTML='';}"
            "const o=document.getElementById('ota');"
            "if(d.ota&&d.ota.state!='idle'){o.style.display='block';"
            "if(d.ota.state=='running')o.innerHTML=`Updating... ${d.ota.progress}%`;"
            "else if(d.ota.state=='pending')o.innerHTML='Update pending...';"
            "else if(d.ota.state=='done')o.innerHTML='Update done. Restarting...';"
            "else if(d.ota.state=='failed')o.innerHTML=`Update failed: ${d.ota.error||'unknown'}`;"
            "}else{o.style.display='none';}"
            "}catch(e){}}"
            "function toast(m,err){const t=document.getElementById('t');t.textContent=m;"
            "t.className='toast show'+(err?' err':'');setTimeout(()=>t.className='toast',2500);}"
            "document.getElementById('f').addEventListener('submit',async e=>{e.preventDefault();"
            "const b=document.getElementById('b');b.disabled=true;b.textContent='Saving...';"
            "try{const r=await fetch('/save',{method:'POST',body:new FormData(e.target)});"
            "if(r.ok)toast('Saved & reconnecting...');else toast('Save failed',true);}"
            "catch(e){toast('Network error',true);}"
            "setTimeout(()=>{b.disabled=false;b.textContent='Save & Reconnect';poll();},1500);});"
            "setInterval(poll,1500);poll();"
            "</script></body></html>");
  return body;
}

static const char* otaStateString() {
  switch (otaState) {
    case OTA_PENDING: return "pending";
    case OTA_RUNNING: return "running";
    case OTA_DONE:    return "done";
    case OTA_FAILED:  return "failed";
    default:          return "idle";
  }
}

static String buildStatusJson() {
  bool stalled = (millis() - lastDataTime > STALL_TIMEOUT_MS);
  String j;
  j.reserve(384);
  j  = "{\"fw\":\"";
  j += FW_VERSION;
  j += "\",\"state\":\"";
  j += statusString(stalled);
  j += "\",\"rx\":";
  j += String((uint32_t)totalBytes);
  j += ",\"uptime\":";
  j += String(millis() / 1000);
  if (updateAvailable) {
    j += ",\"latest\":\"";
    j += latestVersion;
    j += "\",\"releaseUrl\":\"";
    j += releaseUrl;
    j += "\"";
  }
  j += ",\"ota\":{\"state\":\"";
  j += otaStateString();
  j += "\",\"progress\":";
  j += String(otaProgress);
  if (otaError.length()) {
    j += ",\"error\":\"";
    j += otaError;
    j += "\"";
  }
  j += "}}";
  return j;
}

static void applyFormParam(AsyncWebServerRequest *req, const char* name, char* dst, size_t dstSize) {
  if (req->hasParam(name, true)) {
    strlcpy(dst, req->getParam(name, true)->value().c_str(), dstSize);
  }
}

void setupWeb() {
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
    M5.Display.printf("http://%s.local/\n", MDNS_NAME);
  }
  web.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html; charset=utf-8", buildIndexHtml());
  });
  web.on("/status.json", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", buildStatusJson());
  });
  web.on("/save", HTTP_POST, [](AsyncWebServerRequest *req) {
    applyFormParam(req, "host",   host,   sizeof(host));
    applyFormParam(req, "mntpnt", mntpnt, sizeof(mntpnt));
    applyFormParam(req, "user",   user,   sizeof(user));
    applyFormParam(req, "passwd", passwd, sizeof(passwd));
    if (req->hasParam("port", true)) httpPort = req->getParam("port", true)->value().toInt();
    if (req->hasParam("tractor", true)) {
      int idx = req->getParam("tractor", true)->value().toInt();
      if (idx >= 0 && idx < NUM_TRACTOR_PRESETS) currentTractorIdx = idx;
    }
    if (httpPort <= 0 || httpPort > 65535) httpPort = 2101;
    saveSettings();
    reconnectPending = true;
    req->send(200, "text/plain", "saved");
  });
  web.on("/update", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!updateAvailable) {
      req->send(409, "text/plain", "no update available");
      return;
    }
    if (otaState != OTA_IDLE && otaState != OTA_FAILED) {
      req->send(409, "text/plain", "ota in progress");
      return;
    }
    otaState = OTA_PENDING;
    req->send(202, "text/plain", "ota scheduled");
  });
  web.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "text/plain", "not found");
  });
  web.begin();
}

// ---------- Tractor sprite ----------
void initTractor() {
  tractorCanvas.setColorDepth(16);
  tractorCanvas.createSprite(M5.Display.width(), TRACTOR_H + 4);
}

static void drawTractorAt(int sx, int sy) {
  // Facing right, ~32x26. Big rear wheel on left, small front wheel on right.
  const TractorPreset &p = TRACTOR_PRESETS[currentTractorIdx];
  static const uint16_t BROWN = 0x9261;
  // Wheels first (body masks upper halves)
  tractorCanvas.fillCircle(sx +  8, sy + 19, 6, BLACK);          // rear (big)
  tractorCanvas.fillCircle(sx + 25, sy + 20, 4, BLACK);          // front (small)
  tractorCanvas.fillCircle(sx +  8, sy + 19, 3, p.hub);          // rear hub
  tractorCanvas.fillCircle(sx + 25, sy + 20, 2, p.hub);          // front hub
  tractorCanvas.fillCircle(sx +  8, sy + 19, 1, BLACK);          // rear bolt
  // Exhaust pipe (rising up from hood)
  tractorCanvas.fillRect(sx + 15, sy,      2, 12, BROWN);
  tractorCanvas.fillRect(sx + 14, sy + 1,  4,  2, BROWN);        // cap flare
  // Cabin (rear, left)
  tractorCanvas.fillRect(sx +  2, sy + 3, 12,  8, p.body);
  tractorCanvas.fillRect(sx +  1, sy + 2, 14,  2, p.roof);       // roof
  tractorCanvas.drawRect(sx +  4, sy + 5,  8,  5, p.roof);       // window
  // Hood / body (front, right)
  tractorCanvas.fillRect(sx + 15, sy + 11, 15, 7, p.body);
  // Headlight (very front)
  tractorCanvas.fillRect(sx + 28, sy + 12,  2, 3, YELLOW);
}

void renderTractor() {
  tractorCanvas.fillSprite(BLACK);
  drawTractorAt(tractorX, 2);
  tractorCanvas.pushSprite(0, TRACTOR_DISP_Y);
}

static const char* statusString(bool stalled) {
  return
    ntripState == NS_OK         ? (stalled ? "STALLED!" : "OK") :
    ntripState == NS_CONFIG_ERR ? "CFG ERR"                     : "IDLE";
}

static const int INFO_DYN_Y = 100;

void refreshInfoDynamic() {
  bool stalled = (millis() - lastDataTime > STALL_TIMEOUT_MS);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.fillRect(0, INFO_DYN_Y, 320, 32, BLACK);
  M5.Display.setCursor(0, INFO_DYN_Y);
  M5.Display.printf("Status:   %s\n", statusString(stalled));
  M5.Display.printf("RX bytes: %llu\n", totalBytes);
  M5.Display.printf("Uptime:   %lu sec\n", millis() / 1000);
}

void drawInfoPage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.printf("== NTRIP Client FW v%s ==\n\n", FW_VERSION);
  M5.Display.printf("Host:    %s\n", host);
  M5.Display.printf("Port:    %d\n", httpPort);
  M5.Display.printf("Mount:   %s\n", mntpnt);
  M5.Display.printf("User:    %s\n", user[0] ? user : "(none)");
  M5.Display.println();
  M5.Display.printf("WiFi IP: %s\n", WiFi.localIP().toString().c_str());
  M5.Display.printf("mDNS:    http://%s.local/\n", MDNS_NAME);
  M5.Display.printf("Tractor: %s\n", TRACTOR_PRESETS[currentTractorIdx].name);
  refreshInfoDynamic();
  if (updateAvailable) {
    M5.Display.setTextColor(YELLOW, BLACK);
    M5.Display.setCursor(0, 150);
    M5.Display.printf(">> Update: v%s available <<", latestVersion.c_str());
    M5.Display.setTextColor(WHITE, BLACK);
  }
  // Button labels (B becomes "Update" when newer FW available)
  M5.Display.fillRect(0, 220, 320, 20, 0x18C3);
  M5.Display.setTextColor(WHITE, 0x18C3);
  M5.Display.setCursor(8,   226); M5.Display.print("A: Sky");
  if (updateAvailable) {
    M5.Display.setTextColor(YELLOW, 0x18C3);
    M5.Display.setCursor(110, 226); M5.Display.print("B: Update");
    M5.Display.setTextColor(WHITE, 0x18C3);
  } else {
    M5.Display.setCursor(125, 226); M5.Display.print("B: QR");
  }
  M5.Display.setCursor(232, 226); M5.Display.print("C: Sleep");
  M5.Display.setTextColor(WHITE, BLACK);
  renderTractor();
}

// Reusable button bar for Sky/Graph/QR (A label varies)
static void drawNavBar(const char* aLabel, const char* bLabel) {
  M5.Display.fillRect(0, 220, 320, 20, 0x18C3);
  M5.Display.setTextColor(WHITE, 0x18C3);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8,   226); M5.Display.print(aLabel);
  M5.Display.setCursor(125, 226); M5.Display.print(bLabel);
  M5.Display.setCursor(232, 226); M5.Display.print("C: Sleep");
  M5.Display.setTextColor(WHITE, BLACK);
}

// ---------- Sky page ----------
static const int SKY_CX = 160;
static const int SKY_CY = 110;
static const int SKY_R  = 95;

static uint16_t constellationColor(uint8_t cons) {
  switch (cons) {
    case 0: return WHITE;                // GPS
    case 1: return TFT_RED;              // GLONASS
    case 2: return TFT_CYAN;             // Galileo
    case 3: return TFT_YELLOW;           // BeiDou
    case 4: return TFT_MAGENTA;          // QZSS
    default: return TFT_LIGHTGREY;
  }
}

static const char* constellationLabel(uint8_t cons) {
  switch (cons) {
    case 0: return "GPS";
    case 1: return "GLO";
    case 2: return "GAL";
    case 3: return "BDS";
    case 4: return "QZS";
    default: return "?";
  }
}

static void drawSkyGrid() {
  M5.Display.drawCircle(SKY_CX, SKY_CY, SKY_R,       TFT_DARKGREY);
  M5.Display.drawCircle(SKY_CX, SKY_CY, SKY_R * 2/3, TFT_DARKGREY);
  M5.Display.drawCircle(SKY_CX, SKY_CY, SKY_R / 3,   TFT_DARKGREY);
  M5.Display.drawLine(SKY_CX - SKY_R, SKY_CY, SKY_CX + SKY_R, SKY_CY, TFT_DARKGREY);
  M5.Display.drawLine(SKY_CX, SKY_CY - SKY_R, SKY_CX, SKY_CY + SKY_R, TFT_DARKGREY);
  M5.Display.setTextColor(TFT_DARKGREY, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(SKY_CX - 3,        SKY_CY - SKY_R - 10); M5.Display.print("N");
  M5.Display.setCursor(SKY_CX + SKY_R + 3, SKY_CY - 3);          M5.Display.print("E");
  M5.Display.setCursor(SKY_CX - 3,        SKY_CY + SKY_R + 3);  M5.Display.print("S");
  M5.Display.setCursor(SKY_CX - SKY_R - 8, SKY_CY - 3);          M5.Display.print("W");
}

static void plotSats() {
  int counts[5][2] = {0};   // [cons][0=visible, 1=tracked]
  int snrSum = 0, snrCount = 0;
  for (int i = 0; i < satCount; i++) {
    SatInfo &s = sats[i];
    if (s.cons < 5) {
      counts[s.cons][0]++;
      if (s.snr > 0) counts[s.cons][1]++;
    }
    if (s.elev > 90) continue;
    float r = (90 - s.elev) * SKY_R / 90.0f;
    float a = (s.az - 90) * M_PI / 180.0f;
    int x = SKY_CX + (int)(r * cosf(a));
    int y = SKY_CY + (int)(r * sinf(a));
    uint16_t col = constellationColor(s.cons);
    int size = 2 + s.snr / 12;        // SNR-driven dot size
    if (size > 6) size = 6;
    if (s.snr > 0) M5.Display.fillCircle(x, y, size, col);
    else           M5.Display.drawCircle(x, y, 3,    col);
    if (s.snr > 0) { snrSum += s.snr; snrCount++; }
    // PRN label
    M5.Display.setTextColor(col, BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(x + size + 1, y - 3);
    M5.Display.print(s.prn);
  }
  // Summary line at y=200
  M5.Display.setTextSize(1);
  M5.Display.fillRect(0, 200, 320, 16, BLACK);
  M5.Display.setCursor(0, 202);
  int xpos = 0;
  for (int c = 0; c < 5; c++) {
    if (counts[c][0] == 0) continue;
    M5.Display.setTextColor(constellationColor(c), BLACK);
    M5.Display.printf("%s:%d/%d ", constellationLabel(c), counts[c][1], counts[c][0]);
  }
  if (snrCount > 0) {
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.printf(" avg %d", snrSum / snrCount);
  }
}

void drawSkyPage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  if (satCount == 0) {
    M5.Display.println("Sky plot");
    M5.Display.println();
    M5.Display.setTextColor(TFT_DARKGREY, BLACK);
    M5.Display.setCursor(40, SKY_CY);
    M5.Display.print("(waiting for NMEA on RX2...)");
    M5.Display.setTextColor(WHITE, BLACK);
  } else {
    drawSkyGrid();
    plotSats();
  }
  drawNavBar("A: Graph", updateAvailable ? "B: Update" : "B: QR");
}

void refreshSkyPage() {
  M5.Display.fillRect(0, 0, 320, 220, BLACK);
  if (satCount == 0) {
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(0, 0);
    M5.Display.println("Sky plot");
    M5.Display.setTextColor(TFT_DARKGREY, BLACK);
    M5.Display.setCursor(40, SKY_CY);
    M5.Display.print("(waiting for NMEA on RX2...)");
    M5.Display.setTextColor(WHITE, BLACK);
  } else {
    drawSkyGrid();
    plotSats();
  }
}

// ---------- Graph page ----------
void drawGraphPage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.println("RX bytes per 5min  (last 1h)");

  // Find max for scaling
  uint32_t mx = 1;
  for (int i = 0; i < BUCKET_COUNT; i++) if (rxBuckets[i] > mx) mx = rxBuckets[i];

  const int gx = 30, gy = 30, gw = 280, gh = 160;
  M5.Display.drawLine(gx, gy, gx, gy + gh, WHITE);
  M5.Display.drawLine(gx, gy + gh, gx + gw, gy + gh, WHITE);

  int bw = gw / BUCKET_COUNT;
  // Bars: oldest at left, current at right.
  for (int i = 0; i < BUCKET_COUNT; i++) {
    int idx = (bucketHead + 1 + i) % BUCKET_COUNT;  // oldest first
    int h = (int)((uint64_t)rxBuckets[idx] * gh / mx);
    int x = gx + 2 + i * bw;
    bool current = (idx == bucketHead);
    M5.Display.fillRect(x, gy + gh - h, bw - 2, h, current ? TFT_YELLOW : TFT_GREEN);
  }

  // Y-axis max label
  M5.Display.setCursor(0, gy);
  M5.Display.printf("%lu", (unsigned long)mx);
  M5.Display.setCursor(0, gy + gh - 8);
  M5.Display.print("0");

  // X-axis labels
  M5.Display.setCursor(gx, gy + gh + 4);
  M5.Display.print("-1h");
  M5.Display.setCursor(gx + gw - 18, gy + gh + 4);
  M5.Display.print("now");

  drawNavBar("A: Info", updateAvailable ? "B: Update" : "B: QR");
}

void drawUpdateConfirmPage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 20);
  M5.Display.println(" Firmware Update");
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 60);
  M5.Display.printf(" Current: v%s\n", FW_VERSION);
  M5.Display.printf(" Latest:  v%s\n", latestVersion.c_str());
  M5.Display.setCursor(0, 100);
  M5.Display.println(" Download from GitHub and");
  M5.Display.println(" flash. Device will restart.");
  M5.Display.setCursor(0, 150);
  M5.Display.setTextSize(2);
  M5.Display.println(" Proceed?");
  // Button labels
  M5.Display.fillRect(0, 220, 320, 20, 0x18C3);
  M5.Display.setTextColor(WHITE, 0x18C3);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(20,  226); M5.Display.print("A: NO");
  M5.Display.setCursor(220, 226); M5.Display.print("C: YES");
  M5.Display.setTextColor(WHITE, BLACK);
}

void drawUpdatingPage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 20);
  M5.Display.println(" Updating...");
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 60);
  M5.Display.printf(" Downloading v%s\n", latestVersion.c_str());
  M5.Display.setCursor(0, 80);
  M5.Display.printf(" %s\n", FW_BIN_NAME);
}

void refreshUpdatingPage() {
  M5.Display.setTextSize(3);
  M5.Display.setCursor(80, 110);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.printf("%3d%%   ", otaProgress);
  M5.Display.setTextColor(WHITE, BLACK);
  // progress bar
  M5.Display.drawRect(20, 160, 280, 16, WHITE);
  int w = (280 - 2) * otaProgress / 100;
  M5.Display.fillRect(21, 161, w, 14, YELLOW);
  M5.Display.fillRect(21 + w, 161, 280 - 2 - w, 14, BLACK);
  if (otaState == OTA_FAILED) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(RED, BLACK);
    M5.Display.setCursor(0, 200);
    M5.Display.printf(" FAILED: %s        ", otaError.c_str());
    M5.Display.setTextColor(WHITE, BLACK);
  }
}

void drawQrPage() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.println("Scan to open config page:");

  String url = String("http://") + MDNS_NAME + ".local/";
  M5.Display.qrcode(url.c_str(), 70, 18, 180, 3);

  M5.Display.setCursor(0, 205);
  M5.Display.print(url);

  drawNavBar("A: Info", updateAvailable ? "B: Update" : "B: Info");
}

void redrawCurrentPage() {
  switch (currentPage) {
    case PAGE_INFO:           drawInfoPage();          break;
    case PAGE_SKY:            drawSkyPage();           break;
    case PAGE_GRAPH:          drawGraphPage();         break;
    case PAGE_QR:             drawQrPage();            break;
    case PAGE_UPDATE_CONFIRM: drawUpdateConfirmPage(); break;
    case PAGE_UPDATING:       drawUpdatingPage();      break;
  }
}

void sleepDisplay() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  displayAsleep = true;
}

void wakeDisplay() {
  M5.Display.wakeup();
  M5.Display.setBrightness(255);
  displayAsleep = false;
  redrawCurrentPage();
}

void handleButtons() {
  // No buttons during OTA
  if (currentPage == PAGE_UPDATING) return;

  if (M5.BtnA.wasPressed()) {
    if (displayAsleep) { wakeDisplay(); return; }
    if (currentPage == PAGE_UPDATE_CONFIRM) {     // NO -> back to Info
      currentPage = PAGE_INFO;
    } else {
      // Cycle Info -> Sky -> Graph -> Info (skipping QR and special pages)
      switch (currentPage) {
        case PAGE_INFO:  currentPage = PAGE_SKY;   break;
        case PAGE_SKY:   currentPage = PAGE_GRAPH; break;
        case PAGE_GRAPH: currentPage = PAGE_INFO;  break;
        default:         currentPage = PAGE_INFO;  break;     // from QR
      }
    }
    redrawCurrentPage();
  }
  if (M5.BtnB.wasPressed()) {
    if (displayAsleep) { wakeDisplay(); return; }
    if (currentPage == PAGE_UPDATE_CONFIRM) {
      // B is no-op on confirm
    } else if (updateAvailable) {
      currentPage = PAGE_UPDATE_CONFIRM;
      redrawCurrentPage();
    } else {
      currentPage = (currentPage == PAGE_QR) ? PAGE_INFO : PAGE_QR;
      redrawCurrentPage();
    }
  }
  if (M5.BtnC.wasPressed()) {
    if (displayAsleep) { wakeDisplay(); return; }
    if (currentPage == PAGE_UPDATE_CONFIRM) {     // YES -> start OTA
      otaState = OTA_PENDING;
      currentPage = PAGE_UPDATING;
      redrawCurrentPage();
    } else {
      sleepDisplay();
    }
  }
}

// ---------- NTRIP ----------
bool connectNtrip() {
  ntrip_c.stop();
  Serial.printf("Connecting NTRIP: %s:%d/%s\n", host, httpPort, mntpnt);
  bool ok = ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd);
  if (ok) {
    Serial.println("NTRIP connected!");
    ntripState   = NS_OK;
    lastDataTime = millis();
    lastBytes    = totalBytes;
  } else {
    Serial.println("NTRIP connection failed");
    ntripState = NS_CONFIG_ERR;
  }
  return ok;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.begin(uart_bps, SERIAL_8N1, 16, 17);
  initTractor();

  Serial.printf("NTRIP Client FW v%s\n", FW_VERSION);
  loadSettings();
  setupWiFi();
  setupWeb();
  checkLatestRelease();
  connectNtrip();
  redrawCurrentPage();
}

void loop() {
  M5.update();
  handleButtons();

  if (otaState == OTA_PENDING) {
    ntrip_c.stop();
    if (currentPage != PAGE_UPDATING) {
      currentPage = PAGE_UPDATING;
      drawUpdatingPage();
    }
    refreshUpdatingPage();
    runOta();      // blocks; restarts on success
    refreshUpdatingPage();
    return;
  }

  if (reconnectPending) {
    reconnectPending = false;
    connectNtrip();
    redrawCurrentPage();
  }

  while (ntrip_c.available()) {
    char ch = ntrip_c.read();
    Serial2.print(ch);
    totalBytes++;
  }
  Serial2.flush();

  // NMEA from the receiver on Serial2 RX (pin 16)
  while (Serial2.available()) {
    processNmeaChar(Serial2.read());
  }

  unsigned long now = millis();

  if (totalBytes > lastBytes) {
    lastBytes = totalBytes;
    lastDataTime = now;
  }

  bool stalled = (now - lastDataTime > STALL_TIMEOUT_MS);

  if (ntripState == NS_OK && !ntrip_c.connected()) {
    Serial.println("NTRIP disconnected, reconnecting...");
    delay(500);
    connectNtrip();
    redrawCurrentPage();
  }

  updateBuckets();

  if (currentPage == PAGE_INFO && !displayAsleep
      && now - lastTractorTick >= TRACTOR_INTERVAL_MS) {
    lastTractorTick = now;
    bool receiving = (ntripState == NS_OK) && !stalled;
    if (receiving) {
      tractorX += TRACTOR_SPEED;
      if (tractorX > (int)M5.Display.width()) tractorX = -TRACTOR_W;
      renderTractor();
    }
  }

  static unsigned long lastPrint = 0;
  if (now - lastPrint >= 1000) {
    Serial.printf("RTCM bytes: %llu  state=%d  sats=%d%s\n",
                  totalBytes, (int)ntripState, satCount,
                  stalled ? " [STALLED]" : "");
    pruneSats();
    if (!displayAsleep) {
      switch (currentPage) {
        case PAGE_INFO:  refreshInfoDynamic(); break;
        case PAGE_SKY:   refreshSkyPage();    break;
        case PAGE_GRAPH: drawGraphPage();     break;  // full redraw is cheap enough
        default: break;
      }
    }
    lastPrint = now;
  }

  delay(10);
}
