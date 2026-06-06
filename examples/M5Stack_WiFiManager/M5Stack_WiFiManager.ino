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
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "NTRIPClient.h"

NTRIPClient ntrip_c;
WebServer   web(80);
Preferences prefs;

static const char* MDNS_NAME  = "ntrip-client";
static const char* FW_VERSION = "0.3.0";
static const char* FW_REPO    = "yasunorioi/NTRIP-client-for-Arduino";

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

void handleRoot() {
  String stateStr =
    ntripState == NS_OK         ? "connected" :
    ntripState == NS_CONFIG_ERR ? "config error" : "idle";

  String body;
  body.reserve(2560);
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
            "</style></head><body>");
  body += F("<h2>NTRIP Client</h2>");
  body += "<div class='ver'>FW v";
  body += FW_VERSION;
  if (updateAvailable) {
    body += " <a class='upd' href='" + htmlEscape(releaseUrl) + "' target='_blank' rel='noopener'>";
    body += "New: v" + htmlEscape(latestVersion) + " &#8599;</a>";
  }
  body += F("</div>");
  body += "<div class='s'>State: <b>" + stateStr + "</b><br>RX bytes: " + String((uint32_t)totalBytes) + "</div>";
  body += F("<form method='POST' action='/save'>");
  body += "<label>Host</label><input name='host' value='" + htmlEscape(host) + "'>";
  body += F("<div class='row'><div>");
  body += "<label>Port</label><input name='port' type='number' value='" + String(httpPort) + "'>";
  body += F("</div><div>");
  body += "<label>Mountpoint</label><input name='mntpnt' value='" + htmlEscape(mntpnt) + "'>";
  body += F("</div></div>");
  body += "<label>User</label><input name='user' value='" + htmlEscape(user) + "'>";
  body += "<label>Password</label><input name='passwd' type='password' value='" + htmlEscape(passwd) + "'>";
  body += F("<button type='submit'>Save &amp; Reconnect</button></form></body></html>");
  web.send(200, "text/html; charset=utf-8", body);
}

void handleSave() {
  if (web.hasArg("host"))   strlcpy(host,   web.arg("host").c_str(),   sizeof(host));
  if (web.hasArg("port"))   httpPort = web.arg("port").toInt();
  if (web.hasArg("mntpnt")) strlcpy(mntpnt, web.arg("mntpnt").c_str(), sizeof(mntpnt));
  if (web.hasArg("user"))   strlcpy(user,   web.arg("user").c_str(),   sizeof(user));
  if (web.hasArg("passwd")) strlcpy(passwd, web.arg("passwd").c_str(), sizeof(passwd));
  if (httpPort <= 0 || httpPort > 65535) httpPort = 2101;

  saveSettings();
  reconnectPending = true;

  web.sendHeader("Location", "/", true);
  web.send(303, "text/plain", "Saved");
}

void setupWeb() {
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
    M5.Display.printf("http://%s.local/\n", MDNS_NAME);
  }
  web.on("/",     HTTP_GET,  handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
}

void drawHeader() {
  M5.Display.fillScreen(BLACK);
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
}

// ---------- NTRIP ----------
bool connectNtrip() {
  ntrip_c.stop();
  Serial.printf("Connecting NTRIP: %s:%d/%s\n", host, httpPort, mntpnt);
  drawHeader();
  bool ok = ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd);
  if (ok) {
    Serial.println("NTRIP connected!");
    M5.Display.println("NTRIP connected!");
    ntripState   = NS_OK;
    lastDataTime = millis();
    lastBytes    = totalBytes;
  } else {
    Serial.println("NTRIP connection failed");
    M5.Display.println("NTRIP FAILED");
    ntripState = NS_CONFIG_ERR;
  }
  return ok;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.begin(uart_bps, SERIAL_8N1, 16, 17);

  Serial.printf("NTRIP Client FW v%s\n", FW_VERSION);
  loadSettings();
  setupWiFi();
  setupWeb();
  checkLatestRelease();
  connectNtrip();
}

void loop() {
  M5.update();
  web.handleClient();

  if (reconnectPending) {
    reconnectPending = false;
    connectNtrip();
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
    M5.Display.println("\nDISCONNECTED");
    delay(500);
    connectNtrip();
  }

  static unsigned long lastPrint = 0;
  if (now - lastPrint >= 1000) {
    M5.Display.setTextSize(3);
    M5.Display.setCursor(0, 48);
    M5.Display.printf("%llu  ", totalBytes);

    M5.Display.setTextSize(1);
    M5.Display.setCursor(0, 90);
    const char* st =
      ntripState == NS_OK         ? (stalled ? "STALLED!" : "OK") :
      ntripState == NS_CONFIG_ERR ? "CFG ERR"                     : "IDLE";
    M5.Display.printf("Status: %s     ", st);

    M5.Display.setCursor(0, 224);
    M5.Display.setTextSize(2);
    M5.Display.printf("RS232c: %dbps", uart_bps);

    Serial.printf("RTCM bytes: %llu  state=%d%s\n",
                  totalBytes, (int)ntripState, stalled ? " [STALLED]" : "");
    lastPrint = now;
  }

  delay(10);
}
