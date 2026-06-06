/*
 *  NTRIP client for M5Atom (M5Unified + WiFiManager)
 *  - Connects to NTRIP caster and forwards RTCM data to Serial2
 *  - LED: Green=receiving, Red=disconnected/error, Rainbow=stalled, Magenta=NTRIP config error
 *  - Hold button at boot to enter WiFi config portal
 *  - After WiFi connect, open http://ntrip-client.local/ to edit NTRIP host/port/mountpoint/user/passwd
 *    (settings persist in NVS, applied immediately by reconnecting)
 *  - Firmware update check: on boot, queries GitHub Releases and shows a banner on the web UI
 *    if a newer version is available
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

static const char* MDNS_NAME   = "ntrip-client";
static const char* FW_VERSION  = "0.5.0";
static const char* FW_REPO     = "yasunorioi/NTRIP-client-for-Arduino";
static const char* FW_BIN_NAME = "m5atom-wifimanager.bin";

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
uint8_t rainbowHue = 0;

enum NtripState { NS_IDLE, NS_OK, NS_CONFIG_ERR };
NtripState ntripState = NS_IDLE;
bool reconnectPending = false;

String latestVersion;
String releaseUrl;
bool   updateAvailable = false;

// ---- OTA state ----
enum OtaState { OTA_IDLE, OTA_PENDING, OTA_RUNNING, OTA_DONE, OTA_FAILED };
volatile OtaState otaState = OTA_IDLE;
int     otaProgress = 0;
String  otaError;

// ---------- NVS ----------
void loadSettings() {
  prefs.begin("ntrip", true);
  String h = prefs.getString("host",   host);
  int    p = prefs.getInt   ("port",   httpPort);
  String m = prefs.getString("mntpnt", mntpnt);
  String u = prefs.getString("user",   user);
  String w = prefs.getString("passwd", passwd);
  prefs.end();
  strlcpy(host,   h.c_str(), sizeof(host));
  httpPort = p;
  strlcpy(mntpnt, m.c_str(), sizeof(mntpnt));
  strlcpy(user,   u.c_str(), sizeof(user));
  strlcpy(passwd, w.c_str(), sizeof(passwd));
}

void saveSettings() {
  prefs.begin("ntrip", false);
  prefs.putString("host",   host);
  prefs.putInt   ("port",   httpPort);
  prefs.putString("mntpnt", mntpnt);
  prefs.putString("user",   user);
  prefs.putString("passwd", passwd);
  prefs.end();
}

// ---------- LED helpers ----------
void hsvToRgb(uint8_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = h / 43;
  uint8_t remainder = (h - region * 43) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
  switch (region) {
    case 0:  r=v; g=t; b=p; break;
    case 1:  r=q; g=v; b=p; break;
    case 2:  r=p; g=v; b=t; break;
    case 3:  r=p; g=q; b=v; break;
    case 4:  r=t; g=p; b=v; break;
    default: r=v; g=p; b=q; break;
  }
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  M5.Led.setColor(0, r, g, b);
}

void setLedRainbow() {
  uint8_t r, g, b;
  hsvToRgb(rainbowHue, 255, 64, r, g, b);
  setLed(r, g, b);
  rainbowHue += 4;
}

// ---------- WiFi ----------
void setupWiFi() {
  WiFiManager wm;

  bool doManualConfig = false;
  Serial.println("Hold button for WiFi config...");
  for (int i = 0; i < 200; i++) {
    M5.update();
    if (M5.BtnA.isPressed()) {
      doManualConfig = true;
      break;
    }
    delay(10);
  }

  if (doManualConfig) {
    Serial.println("Starting WiFi config portal");
    setLed(0, 0, 0x40);
    wm.startConfigPortal("NTRIP-Client");
  } else {
    Serial.println("WiFi connecting...");
    setLed(0x40, 0x40, 0);
    wm.autoConnect("NTRIP-Client");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed, restarting...");
    setLed(0x40, 0, 0);
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

  setLed(0x40, 0, 0x40); // magenta during OTA

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) { otaError = "http.begin failed"; otaState = OTA_FAILED; return; }
  http.addHeader("User-Agent", "NTRIP-Client-OTA");
  int code = http.GET();
  if (code != 200)             { otaError = "HTTP " + String(code); otaState = OTA_FAILED; http.end(); return; }
  int total = http.getSize();
  if (total <= 0)              { otaError = "no content-length";    otaState = OTA_FAILED; http.end(); return; }
  if (!Update.begin((size_t)total)) {
    otaError = String("Update.begin: ") + Update.errorString();
    otaState = OTA_FAILED; http.end(); return;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
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
    } else {
      delay(1);
    }
    if (millis() - lastYield > 50) { yield(); lastYield = millis(); }
  }
  http.end();
  if (written != total) { otaError = "short read"; Update.abort(); otaState = OTA_FAILED; return; }
  if (!Update.end(true)) {
    otaError = String("Update.end: ") + Update.errorString();
    otaState = OTA_FAILED; return;
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

static const char* statusString() {
  bool stalled = (millis() - lastDataTime > STALL_TIMEOUT_MS);
  return
    ntripState == NS_OK         ? (stalled ? "STALLED" : "OK") :
    ntripState == NS_CONFIG_ERR ? "CFG ERR"                    : "IDLE";
}

static String buildIndexHtml() {
  String body;
  body.reserve(3328);
  body += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>NTRIP Client</title>"
            "<style>body{font-family:sans-serif;max-width:480px;margin:1em auto;padding:0 1em}"
            "label{display:block;margin:.6em 0 .2em}input{width:100%;padding:.4em;font-size:1em}"
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
  String j;
  j.reserve(384);
  j  = "{\"fw\":\"";
  j += FW_VERSION;
  j += "\",\"state\":\"";
  j += statusString();
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
    if (httpPort <= 0 || httpPort > 65535) httpPort = 2101;
    saveSettings();
    reconnectPending = true;
    req->send(200, "text/plain", "saved");
  });
  web.on("/update", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!updateAvailable) { req->send(409, "text/plain", "no update available"); return; }
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

// ---------- NTRIP ----------
bool connectNtrip() {
  ntrip_c.stop();
  Serial.printf("Connecting NTRIP: %s:%d/%s\n", host, httpPort, mntpnt);
  bool ok = ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd);
  if (ok) {
    Serial.println("NTRIP connected!");
    ntripState  = NS_OK;
    lastDataTime = millis();
    lastBytes    = totalBytes;
    setLed(0, 0x40, 0);
  } else {
    Serial.println("NTRIP connection failed");
    ntripState = NS_CONFIG_ERR;
    setLed(0x40, 0, 0x40); // magenta = check settings
  }
  return ok;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 22, 19);

  setLed(0x40, 0, 0);
  Serial.printf("NTRIP Client FW v%s\n", FW_VERSION);
  loadSettings();
  setupWiFi();
  setupWeb();
  checkLatestRelease();
  connectNtrip();
}

void loop() {
  M5.update();

  if (otaState == OTA_PENDING) {
    ntrip_c.stop();
    runOta();
    return;
  }

  if (reconnectPending) {
    reconnectPending = false;
    connectNtrip();
  }

  while (ntrip_c.available()) {
    char ch = ntrip_c.read();
    Serial2.print(ch);
    totalBytes++;
  }

  unsigned long now = millis();

  if (totalBytes > lastBytes) {
    lastBytes = totalBytes;
    lastDataTime = now;
    if (ntripState == NS_OK) setLed(0, 0x40, 0);
  }

  if (ntripState == NS_OK) {
    if (now - lastDataTime > STALL_TIMEOUT_MS) {
      setLedRainbow();
    }
    if (!ntrip_c.connected()) {
      Serial.println("NTRIP disconnected, reconnecting...");
      setLed(0x40, 0, 0);
      delay(500);
      connectNtrip();
    }
  }

  static unsigned long lastPrint = 0;
  if (now - lastPrint >= 1000) {
    Serial.printf("RTCM bytes: %llu  state=%d\n", totalBytes, (int)ntripState);
    lastPrint = now;
  }

  delay(10);
}
