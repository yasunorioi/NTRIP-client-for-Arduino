#include "NTRIPConfigPortal.h"

#if NTRIP_CONFIG_PORTAL_AVAILABLE

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

namespace {

constexpr byte   DNS_PORT  = 53;
constexpr uint16_t HTTP_PORT = 80;

// 小さい1ページHTML。スタイルはインラインで完結させて外部依存ゼロ。
// %FIELD% プレースホルダを replaceField() で穴埋めする。
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ja"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NTRIP Setup</title>
<style>
body{font-family:system-ui,sans-serif;max-width:480px;margin:1em auto;padding:0 1em;color:#222}
h1{font-size:1.2em}
label{display:block;margin:.6em 0 .2em;font-weight:600}
input[type=text],input[type=password],input[type=number]{width:100%;padding:.5em;font-size:1em;box-sizing:border-box}
fieldset{border:1px solid #ccc;padding:.6em;margin:.8em 0}
legend{font-weight:600}
.row{display:flex;gap:.5em}.row>div{flex:1}
.btn{padding:.7em 1.2em;font-size:1em;border:0;border-radius:.3em;cursor:pointer}
.primary{background:#2563eb;color:#fff}
.danger{background:#dc2626;color:#fff;margin-left:.5em}
small{color:#666}
</style></head><body>
<h1>NTRIP Setup</h1>
<form method="post" action="/save">
<fieldset><legend>NTRIP caster</legend>
<label>Host <input name="host" type="text" value="%HOST%" required></label>
<div class="row">
<div><label>Port <input name="port" type="number" min="1" max="65535" value="%PORT%" required></label></div>
<div><label>Mountpoint <input name="mntpnt" type="text" value="%MNTPNT%" required></label></div>
</div>
<label>User <input name="user" type="text" value="%USER%"></label>
<label>Password <input name="passwd" type="password" value="%PASSWD%"></label>
<small>User/Password は anonymous mountpoint なら空欄でOK</small>
</fieldset>

<fieldset><legend>VRS</legend>
<label><input name="vrs" type="checkbox" %VRS_CHECKED%> VRS 有効 (GGA を caster に定期送信)</label>
<label>GGA 送信間隔 (sec) <input name="ggaSec" type="number" min="1" max="3600" value="%GGA_SEC%"></label>
<label><input name="ggaRx" type="checkbox" %GGARX_CHECKED%> 受信機の NMEA から GGA を流用 (Bridge のみ)</label>
<small>受信機 GGA を使わない場合は下の手動位置を使う</small>
<div class="row">
<div><label>手動 Lat <input name="lat" type="text" value="%LAT%"></label></div>
<div><label>手動 Lon <input name="lon" type="text" value="%LON%"></label></div>
<div><label>手動 Alt (m) <input name="alt" type="text" value="%ALT%"></label></div>
</div>
</fieldset>

<button class="btn primary" type="submit">Save & Reboot</button>
<button class="btn danger" formaction="/reset" formnovalidate>Factory Reset</button>
</form>
</body></html>
)HTML";

String replaceField(String html, const NTRIPConfig& cfg) {
  html.replace("%HOST%",    cfg.host);
  html.replace("%PORT%",    String((uint32_t)cfg.port));
  html.replace("%MNTPNT%",  cfg.mountpoint);
  html.replace("%USER%",    cfg.user);
  html.replace("%PASSWD%",  cfg.passwd);
  html.replace("%VRS_CHECKED%",   cfg.vrsEnabled    ? "checked" : "");
  html.replace("%GGARX_CHECKED%", cfg.useReceiverGGA ? "checked" : "");
  html.replace("%GGA_SEC%", String((uint32_t)cfg.ggaIntervalSec));
  // double を文字列化 (小数 7 桁あれば cm 精度)
  char buf[24];
  snprintf(buf, sizeof(buf), "%.7f", cfg.manualLat); html.replace("%LAT%", buf);
  snprintf(buf, sizeof(buf), "%.7f", cfg.manualLon); html.replace("%LON%", buf);
  snprintf(buf, sizeof(buf), "%.2f", cfg.manualAltMeters); html.replace("%ALT%", buf);
  return html;
}

}  // namespace

NTRIPConfigPortal::NTRIPConfigPortal()  = default;
NTRIPConfigPortal::~NTRIPConfigPortal() = default;

bool NTRIPConfigPortal::run(NTRIPConfig& cfg,
                            const String& apSsid,
                            const String& apPassword,
                            uint32_t timeoutMs) {
  Serial.printf("[Portal] Starting AP '%s' (timeout %u ms)\n",
                apSsid.c_str(), (unsigned)timeoutMs);

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_AP);
  if (apPassword.length() >= 8) {
    WiFi.softAP(apSsid.c_str(), apPassword.c_str());
  } else {
    WiFi.softAP(apSsid.c_str()); // open AP (パスワード短すぎる場合)
  }
  delay(100);
  IPAddress apIp = WiFi.softAPIP();
  Serial.print("[Portal] AP IP: "); Serial.println(apIp);

  // キャプティブポータル: 全ドメインを自分の IP に解決
  DNSServer dns;
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(DNS_PORT, "*", apIp);

  AsyncWebServer server(HTTP_PORT);

  // 保存完了したか
  bool   saved   = false;
  bool   reset   = false;
  String formHtml; // ハンドラ間で共有しない (各リクエスト毎に再生成)

  server.on("/", HTTP_GET, [&cfg](AsyncWebServerRequest* req) {
    String body = FPSTR(INDEX_HTML);
    req->send(200, "text/html", replaceField(body, cfg));
  });

  // Android 8+ / iOS / Windows のキャプティブポータル検出 URL を全部 / にリダイレクト
  auto captiveRedirect = [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(302, "text/plain", "");
    r->addHeader("Location", "/");
    req->send(r);
  };
  server.on("/generate_204", HTTP_GET, captiveRedirect);   // Android
  server.on("/hotspot-detect.html", HTTP_GET, captiveRedirect); // iOS
  server.on("/connecttest.txt", HTTP_GET, captiveRedirect);     // Windows
  server.on("/ncsi.txt", HTTP_GET, captiveRedirect);
  server.on("/redirect", HTTP_GET, captiveRedirect);

  server.on("/save", HTTP_POST, [&cfg, &saved](AsyncWebServerRequest* req) {
    auto getStr = [&](const char* name) -> String {
      if (req->hasParam(name, true)) {
        return req->getParam(name, true)->value();
      }
      return String();
    };
    auto getBool = [&](const char* name) -> bool {
      return req->hasParam(name, true); // checkbox は送られたかどうかで判定
    };

    cfg.host           = getStr("host");
    cfg.port           = (uint16_t)getStr("port").toInt();
    if (cfg.port == 0) cfg.port = 2101;
    cfg.mountpoint     = getStr("mntpnt");
    cfg.user           = getStr("user");
    cfg.passwd         = getStr("passwd");
    cfg.vrsEnabled     = getBool("vrs");
    cfg.useReceiverGGA = getBool("ggaRx");
    uint32_t sec       = (uint32_t)getStr("ggaSec").toInt();
    if (sec < 1)    sec = 10;
    if (sec > 3600) sec = 3600;
    cfg.ggaIntervalSec = (uint16_t)sec;
    cfg.manualLat        = getStr("lat").toDouble();
    cfg.manualLon        = getStr("lon").toDouble();
    cfg.manualAltMeters  = getStr("alt").toFloat();

    if (!cfg.save()) {
      req->send(500, "text/plain", "Failed to save settings");
      return;
    }
    saved = true;
    req->send(200, "text/html",
              "<h1>Saved</h1><p>Device will restart in a few seconds...</p>");
  });

  server.on("/reset", HTTP_POST, [&cfg, &reset](AsyncWebServerRequest* req) {
    NTRIPConfig::clear();
    cfg = NTRIPConfig();
    reset = true;
    req->send(200, "text/html",
              "<h1>Factory reset done</h1>"
              "<p>Device will restart. Reconnect and reconfigure.</p>");
  });

  // 全てのその他リクエストもキャプティブ用に / にリダイレクト
  server.onNotFound([&cfg](AsyncWebServerRequest* req) {
    String body = FPSTR(INDEX_HTML);
    req->send(200, "text/html", replaceField(body, cfg));
  });

  server.begin();
  Serial.println("[Portal] HTTP server up");

  unsigned long t0   = millis();
  bool exitRequested = false;
  while (!exitRequested) {
    dns.processNextRequest();
    delay(10);
    if (saved || reset) {
      // クライアントへのレスポンス送信を1秒ほど待つ (Async なので即 close すると応答取りこぼし)
      delay(1000);
      exitRequested = true;
    }
    if (timeoutMs > 0 && (millis() - t0) >= timeoutMs) {
      Serial.println("[Portal] Timeout, exiting without save.");
      exitRequested = true;
    }
  }

  server.end();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);
  return saved || reset;
}

#endif  // NTRIP_CONFIG_PORTAL_AVAILABLE
