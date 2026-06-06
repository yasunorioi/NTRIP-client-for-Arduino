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

static const char* MDNS_NAME      = "ntrip-client";
static const char* FW_VERSION     = "0.5.1";
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
enum DisplayPage { PAGE_MAIN, PAGE_INFO, PAGE_QR, PAGE_UPDATE_CONFIRM, PAGE_UPDATING };
DisplayPage currentPage   = PAGE_INFO;
bool        displayAsleep = false;

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

void drawHeader() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.printf("ntrip://%s:%d/%s\n", host, httpPort, mntpnt);
  M5.Display.printf("IP: %s  mDNS: %s.local\n", WiFi.localIP().toString().c_str(), MDNS_NAME);
  if (updateAvailable) {
    M5.Display.setTextColor(YELLOW, BLACK);
    M5.Display.printf("FW: v%s  >> UPDATE: v%s <<\n", FW_VERSION, latestVersion.c_str());
    M5.Display.setTextColor(WHITE, BLACK);
  } else {
    M5.Display.printf("FW: v%s\n", FW_VERSION);
  }
  renderTractor();
}

void drawMainDynamic() {
  bool stalled = (millis() - lastDataTime > STALL_TIMEOUT_MS);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(0, 48);
  M5.Display.printf("%llu  ", totalBytes);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 90);
  M5.Display.printf("Status: %s     ", statusString(stalled));
}

void drawMainPage() {
  drawHeader();
  drawMainDynamic();
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
  M5.Display.setCursor(8,   226); M5.Display.print("A: Main");
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

  M5.Display.fillRect(0, 220, 320, 20, 0x18C3);
  M5.Display.setTextColor(WHITE, 0x18C3);
  M5.Display.setCursor(8,   226); M5.Display.print("A: Info");
  M5.Display.setCursor(125, 226); M5.Display.print("B: Main");
  M5.Display.setCursor(232, 226); M5.Display.print("C: Sleep");
  M5.Display.setTextColor(WHITE, BLACK);
}

void redrawCurrentPage() {
  switch (currentPage) {
    case PAGE_MAIN:           drawMainPage();          break;
    case PAGE_INFO:           drawInfoPage();          break;
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
    if (currentPage == PAGE_UPDATE_CONFIRM) {  // NO -> back to Info
      currentPage = PAGE_INFO;
      redrawCurrentPage();
    } else {
      currentPage = (currentPage == PAGE_INFO) ? PAGE_MAIN : PAGE_INFO;
      redrawCurrentPage();
    }
  }
  if (M5.BtnB.wasPressed()) {
    if (displayAsleep) { wakeDisplay(); return; }
    if (currentPage == PAGE_INFO && updateAvailable) {
      currentPage = PAGE_UPDATE_CONFIRM;
      redrawCurrentPage();
    } else if (currentPage == PAGE_UPDATE_CONFIRM) {
      // B is no-op on confirm
    } else {
      currentPage = (currentPage == PAGE_QR) ? PAGE_MAIN : PAGE_QR;
      redrawCurrentPage();
    }
  }
  if (M5.BtnC.wasPressed()) {
    if (displayAsleep) { wakeDisplay(); return; }
    if (currentPage == PAGE_UPDATE_CONFIRM) {  // YES -> start OTA
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

  if ((currentPage == PAGE_MAIN || currentPage == PAGE_INFO) && !displayAsleep
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
    Serial.printf("RTCM bytes: %llu  state=%d%s\n",
                  totalBytes, (int)ntripState, stalled ? " [STALLED]" : "");
    if (!displayAsleep) {
      if (currentPage == PAGE_MAIN)      drawMainDynamic();
      else if (currentPage == PAGE_INFO) refreshInfoDynamic();
    }
    lastPrint = now;
  }

  delay(10);
}
