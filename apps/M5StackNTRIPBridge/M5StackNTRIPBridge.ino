/*
 *  NTRIP <-> RTK受信機 ブリッジ版 (M5Stack Basic)
 *
 *  M5AtomNTRIPBridge の M5Stack 版。
 *  WiFi → NTRIP → Serial2(PORT.A G22 TX) → 外部RTK受信機
 *  外部RTK受信機 → Serial2(PORT.A G21 RX) → NMEA → Serial1(G17 TX) → RS232F DB9 → 外部
 *
 *  ピン配置:
 *    Serial2  RX: G21  TX: G22   PORT.A (RTK受信機との双方向)
 *      → Atom 版 Grove ケーブル互換のため、Atom と同じ並びを 21/22 に揃えている。
 *    Serial1  RX: G16  TX: G17   M5Stack RS232F Module 13.2 (DB9)
 *      → 受信機から拾った NMEA を外部に流す。RX は今は使わないが将来用に確保。
 *
 *  ・WiFi接続は WiFiManager (tzapu/WiFiManager)
 *    起動時に BtnA を 2 秒押しでその場で設定ポータルに入る。
 *  ・指数バックオフ + ジッター付きで再接続(回線不安定対策)
 *  ・データストール(無音)検知で能動再接続。通常失敗は ESP.restart() しない。
 *  ・LCD は RTCM/NMEA レートと状態を 1 秒ごとに更新。
 */
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <WiFiManager.h>
#include "NTRIPClient.h"
#include "NTRIPConfig.h"
#include "NTRIPConfigPortal.h"
#include "GitHubRelease.h"

// CI が -DFIRMWARE_VERSION='"vX.Y.Z"' で上書きする。デフォルトは "dev"。
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// リリース情報を取りに行く GitHub リポジトリと OTA ダウンロードに使う app 名
constexpr const char* GH_REPO  = "yasunorioi/NTRIP-client-for-Arduino";
constexpr const char* APP_NAME = "M5StackNTRIPBridge";

// ---- NTRIP 設定 ----
// 値は NVS から起動時にロード。空ならポータルを強制起動。
NTRIPConfig g_cfg;

// 設定ポータル AP 情報 (SSID は chip ID 4 桁付き)
String g_portalSsid;
const char*    PORTAL_PASSWORD    = "configme123";
const uint32_t PORTAL_TIMEOUT_MS  = 5UL * 60UL * 1000UL;  // 5 分

NTRIPClient ntrip_c;

// ---- UART ピン ----
constexpr int PORTA_RX_PIN  = 21;  // RTK受信機 → M5Stack
constexpr int PORTA_TX_PIN  = 22;  // M5Stack → RTK受信機
constexpr int RS232F_RX_PIN = 16;  // (将来用、現状未使用)
constexpr int RS232F_TX_PIN = 17;  // NMEA を外部へ
constexpr int UART_BPS      = 115200;

// Serial1 をオブジェクトとして HardwareSerial(1) で使う (UART1)
HardwareSerial SerialRS232(1);

// ---- 再接続バックオフ設定 ----
const unsigned long INITIAL_BACKOFF_MS = 5UL * 1000UL;
const unsigned long MAX_BACKOFF_MS     = 10UL * 60UL * 1000UL;
const unsigned long DATA_TIMEOUT_MS    = 30UL * 1000UL;
const unsigned long WIFI_RETRY_MS      = 30UL * 1000UL;
const unsigned long REPORT_INTERVAL_MS = 1UL * 1000UL;

// ---- ランタイム状態 ----
enum class AppState : uint8_t { Connecting, Online, Backoff, Stalled, WifiDown };
AppState      state               = AppState::Connecting;

unsigned long lastDataTime         = 0;
unsigned long lastConnectAttempt   = 0;
unsigned long backoffMs            = 0;
int           consecutiveFailures  = 0;
bool          ntripConnected       = false;

uint64_t      rtcmBytesTotal       = 0;
uint64_t      rtcmBytesSinceReport = 0;
uint64_t      nmeaBytesTotal       = 0;
uint64_t      nmeaBytesSinceReport = 0;
unsigned long lastReportTime       = 0;
float         lastRtcmBps          = 0.0f;
float         lastNmeaBps          = 0.0f;

// ---- トラクターアニメーション ----
// RTCM3 受信量に比例して画面下部をトラクターが左→右に進む。
// 画面最下部 (y=212-228) はボタンラベルのために空けたので、トラクターは
// その上 (y=190-208 程度) に。データが止まれば自然に静止する。
constexpr int TRACTOR_W            = 24;
constexpr int TRACTOR_H            = 16;
constexpr int TRACTOR_Y            = 190;
constexpr uint64_t BYTES_PER_PIXEL = 200;  // 200 B/px → 1KB/s で 5 px/秒

// ---- 画面切替 ----
// 0 = 通常ステータス画面 (RTCM/NMEA レート + トラクター)
// 1 = デバイス情報画面 (NTRIP 設定値、IP、MAC、Chip ID、FW、partition)
// BtnB 短押しでトグル。BtnC OTA 中は別の overlay 表示で一時的に上書きする。
volatile int currentScreen = 0;

int           lastTractorX    = -TRACTOR_W;
unsigned long lastTractorTick = 0;

// ---- プロトタイプ ----
void setupWiFi();
void pumpNmeaToRs232f();
void drawHeader();
void drawStatus();
void drawTractor(int x, int y);
void updateTractor(uint64_t totalBytes);
void scheduleRetry(bool increase);
void handleWifiDown();
const char* stateLabel(AppState s);
uint16_t stateColor(AppState s);
void runConfigPortal();
void drawPortalScreen();
void checkPortalButton();
void checkReleaseButton();
void showReleaseInfo();
void checkScreenSwitchButton();
void drawCurrentScreen();
void drawDeviceInfoScreen();
void drawButtonLabels();

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.begin(UART_BPS, SERIAL_8N1, PORTA_RX_PIN, PORTA_TX_PIN);
  SerialRS232.begin(UART_BPS, SERIAL_8N1, RS232F_RX_PIN, RS232F_TX_PIN);

  // Chip ID 末尾 4 桁でユニークな AP 名にする
  uint64_t chipId = ESP.getEfuseMac();
  char ssidBuf[24];
  snprintf(ssidBuf, sizeof(ssidBuf), "NTRIP-Bridge-%04X", (uint16_t)(chipId & 0xFFFF));
  g_portalSsid = ssidBuf;

  // NVS から NTRIP 設定をロード
  g_cfg.load();

  M5.Display.setRotation(1);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);
  M5.Display.println("M5Stack NTRIP Bridge");

  setupWiFi();

  // NTRIP 設定が未保存ならポータルを起動
  if (!g_cfg.isComplete()) {
    Serial.println("NTRIP 設定が空です。設定ポータルを起動します。");
    runConfigPortal();
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(200);
    }
  }

  randomSeed(esp_random());
  lastConnectAttempt = millis() - INITIAL_BACKOFF_MS;
  backoffMs          = 0;
  state              = AppState::Connecting;

  drawCurrentScreen();
  lastReportTime = millis();
}

void loop() {
  M5.update();

  // BtnB 長押し=設定ポータル / BtnB 短押し=画面切替 / BtnC 短押し=Update
  checkPortalButton();
  checkScreenSwitchButton();
  checkReleaseButton();

  // 受信機からの NMEA は NTRIP 状態に関係なく常に RS232F 側へ流す
  pumpNmeaToRs232f();

  // 1) WiFi 状態チェック
  if (WiFi.status() != WL_CONNECTED) {
    ntripConnected = false;
    state          = AppState::WifiDown;
    drawStatus();
    handleWifiDown();
    return;
  }

  // 2) NTRIP 接続中なら RTCM を Serial2(PORT.A) → 受信機 へ流す
  if (ntripConnected) {
    while (ntrip_c.available()) {
      char ch = ntrip_c.read();
      Serial2.write((uint8_t)ch);
      lastDataTime = millis();
      rtcmBytesTotal++;
      rtcmBytesSinceReport++;
    }
    Serial2.flush();
    updateTractor(rtcmBytesTotal);

    unsigned long now = millis();

    if (now - lastReportTime >= REPORT_INTERVAL_MS) {
      unsigned long elapsed = now - lastReportTime;
      lastRtcmBps = (float)rtcmBytesSinceReport * 1000.0f / (float)elapsed;
      lastNmeaBps = (float)nmeaBytesSinceReport * 1000.0f / (float)elapsed;
      Serial.printf("[BRIDGE] %lums  RTCM in %llu B (%.1f B/s, total %llu)  "
                    "NMEA out %llu B (%.1f B/s, total %llu)\n",
                    elapsed,
                    (unsigned long long)rtcmBytesSinceReport, lastRtcmBps,
                    (unsigned long long)rtcmBytesTotal,
                    (unsigned long long)nmeaBytesSinceReport, lastNmeaBps,
                    (unsigned long long)nmeaBytesTotal);
      rtcmBytesSinceReport = 0;
      nmeaBytesSinceReport = 0;
      lastReportTime       = now;

      state = (now - lastDataTime > 5000) ? AppState::Stalled : AppState::Online;
      drawStatus();
    }

    if (now - lastDataTime > DATA_TIMEOUT_MS) {
      Serial.println("RTCM 無音。NTRIP を切断して再接続。");
      ntrip_c.stop();
      ntripConnected = false;
      scheduleRetry(false);
      state = AppState::Backoff;
      drawStatus();
    }
    delay(5);
    return;
  }

  // 3) バックオフ待機
  if (millis() - lastConnectAttempt < backoffMs) {
    if (state != AppState::Backoff) {
      state = AppState::Backoff;
      drawStatus();
    }
    delay(200);
    return;
  }

  // 4) 再接続試行
  state = AppState::Connecting;
  drawStatus();
  lastConnectAttempt = millis();
  Serial.printf("NTRIP接続試行 %s:%u/%s (連続失敗: %d回)\n",
                g_cfg.host.c_str(), g_cfg.port, g_cfg.mountpoint.c_str(),
                consecutiveFailures);

  ntrip_c.stop();
  delay(50);
  int portTmp = g_cfg.port;
  if (ntrip_c.reqRaw(
        (char*)g_cfg.host.c_str(), portTmp,
        (char*)g_cfg.mountpoint.c_str(),
        (char*)g_cfg.user.c_str(),
        (char*)g_cfg.passwd.c_str())) {
    Serial.println("NTRIP接続成功");
    lastDataTime         = millis();
    consecutiveFailures  = 0;
    backoffMs            = 0;
    ntripConnected       = true;
    rtcmBytesSinceReport = 0;
    nmeaBytesSinceReport = 0;
    lastReportTime       = millis();
    state                = AppState::Online;
    drawStatus();
  } else {
    Serial.println("NTRIP接続失敗");
    scheduleRetry(true);
    state = AppState::Backoff;
    drawStatus();
  }
}

void pumpNmeaToRs232f() {
  while (Serial2.available()) {
    int b = Serial2.read();
    if (b < 0) break;
    SerialRS232.write((uint8_t)b);
    nmeaBytesTotal++;
    nmeaBytesSinceReport++;
  }
}

// WiFiManager のポータル AP が立った瞬間に呼ばれる。LCD に WiFi-join QR
// を出してスマホからすぐ参加できるようにする。
// 引数の ssid は WiFiManager に渡したものをここに同期させる。
void drawWifiManagerQr(const char* ssid) {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("WiFi SETUP");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 24);
  M5.Display.printf("SSID: %s\n", ssid);
  M5.Display.println("PASS: (open)");
  M5.Display.println("URL : http://192.168.4.1/");
  M5.Display.println("");
  M5.Display.println("Scan QR to join WiFi");

  // open AP なので nopass。WIFI: の標準形式。
  String wifiQr = "WIFI:T:nopass;S:" + String(ssid) + ";;";
  M5.Display.qrcode(wifiQr.c_str(), 170, 80, 140, 6);
}

void setupWiFi() {
  M5.Display.setTextSize(2);
  M5.Display.println("");
  M5.Display.println("Hold BtnA for");
  M5.Display.println("config portal");

  bool doManualConfig = false;
  unsigned long t0 = millis();
  while (millis() - t0 < 2000) {
    M5.update();
    if (M5.BtnA.isPressed()) {
      doManualConfig = true;
      break;
    }
    delay(10);
  }

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  // ポータル AP が立った瞬間に LCD に QR + AP情報を表示
  wm.setAPCallback([](WiFiManager* /*mgr*/) { drawWifiManagerQr("NTRIP-Bridge"); });

  M5.Display.println("");
  if (doManualConfig) {
    wm.startConfigPortal("NTRIP-Bridge");
  } else {
    M5.Display.println("Connecting WiFi...");
    if (!wm.autoConnect("NTRIP-Bridge")) {
      Serial.println("WiFi接続失敗。設定ポータルへ。");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi NOT connected. Will retry in loop().");
  }
}

void drawHeader() {
  if (currentScreen != 0) return;
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.printf("ntrip://%s:%u/%s\n",
                    g_cfg.host.c_str(), g_cfg.port, g_cfg.mountpoint.c_str());
  M5.Display.printf("IP %s  A=%d/%d  RS232F=%d/%d\n",
                    WiFi.localIP().toString().c_str(),
                    PORTA_RX_PIN, PORTA_TX_PIN,
                    RS232F_RX_PIN, RS232F_TX_PIN);
}

void drawStatus() {
  if (currentScreen != 0) return;
  M5.Display.setTextSize(3);
  M5.Display.setCursor(0, 30);
  M5.Display.setTextColor(stateColor(state), BLACK);
  M5.Display.printf("%-10s", stateLabel(state));

  M5.Display.setTextSize(2);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 70);
  M5.Display.printf("RTCM %6.1f B/s    ", lastRtcmBps);
  M5.Display.setCursor(0, 95);
  M5.Display.printf("NMEA %6.1f B/s    ", lastNmeaBps);

  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 130);
  M5.Display.printf("RTCM total %llu B    ", (unsigned long long)rtcmBytesTotal);
  M5.Display.setCursor(0, 145);
  M5.Display.printf("NMEA total %llu B    ", (unsigned long long)nmeaBytesTotal);

  M5.Display.setCursor(0, 175);
  if (state == AppState::Backoff) {
    unsigned long remaining = (backoffMs > (millis() - lastConnectAttempt))
                                  ? (backoffMs - (millis() - lastConnectAttempt))
                                  : 0;
    M5.Display.printf("Retry in %lus (fails %d)    ",
                      remaining / 1000UL, consecutiveFailures);
  } else {
    M5.Display.printf("Fails: %d              ", consecutiveFailures);
  }
}

const char* stateLabel(AppState s) {
  switch (s) {
    case AppState::Connecting: return "CONNECT";
    case AppState::Online:     return "ONLINE";
    case AppState::Backoff:    return "BACKOFF";
    case AppState::Stalled:    return "STALLED";
    case AppState::WifiDown:   return "WIFI";
  }
  return "?";
}

uint16_t stateColor(AppState s) {
  switch (s) {
    case AppState::Online:     return GREEN;
    case AppState::Stalled:    return ORANGE;
    case AppState::Backoff:    return CYAN;
    case AppState::Connecting: return YELLOW;
    case AppState::WifiDown:   return RED;
  }
  return WHITE;
}

void drawTractor(int x, int y) {
  // 24x16 のサイドビュー: 排気煙突 + キャブ + 車体 + 前後輪
  M5.Display.fillRect(x,      y + 8, 24, 5, YELLOW);
  M5.Display.fillRect(x + 10, y,      8, 8, YELLOW);
  M5.Display.drawRect(x + 10, y,      8, 8, DARKGREY);
  M5.Display.fillRect(x + 8,  y + 1,  2, 5, DARKGREY);
  M5.Display.fillCircle(x + 19, y + 14, 3, DARKGREY);
  M5.Display.fillCircle(x + 19, y + 14, 1, BLACK);
  M5.Display.fillCircle(x + 5,  y + 14, 2, DARKGREY);
}

void updateTractor(uint64_t totalBytes) {
  if (currentScreen != 0) return;
  unsigned long now = millis();
  if (now - lastTractorTick < 50) return;
  lastTractorTick = now;

  int w = M5.Display.width();
  int x = (int)((totalBytes / BYTES_PER_PIXEL) % (uint64_t)w);
  if (x == lastTractorX) return;

  M5.Display.fillRect(0, TRACTOR_Y, w, TRACTOR_H + 2, BLACK);
  drawTractor(x, TRACTOR_Y);
  lastTractorX = x;
}

void scheduleRetry(bool increase) {
  if (increase) {
    consecutiveFailures++;
    if (backoffMs == 0) {
      backoffMs = INITIAL_BACKOFF_MS;
    } else {
      backoffMs *= 2;
      if (backoffMs > MAX_BACKOFF_MS) backoffMs = MAX_BACKOFF_MS;
    }
    long jitterRange = (long)(backoffMs / 5);
    long jitter      = random(-jitterRange, jitterRange + 1);
    long withJitter  = (long)backoffMs + jitter;
    if (withJitter < (long)INITIAL_BACKOFF_MS) withJitter = INITIAL_BACKOFF_MS;
    backoffMs = (unsigned long)withJitter;
  } else {
    if (backoffMs < INITIAL_BACKOFF_MS) backoffMs = INITIAL_BACKOFF_MS;
  }
  Serial.printf("次回試行まで %lu 秒待機\n", backoffMs / 1000UL);
  lastConnectAttempt = millis();
}

void handleWifiDown() {
  Serial.println("WiFi切断中。再接続を待ちます。");
  ntrip_c.stop();
  WiFi.disconnect();
  delay(100);
  WiFi.reconnect();

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_RETRY_MS) {
    // 待機中も NMEA は流す
    pumpNmeaToRs232f();
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi再接続OK");
    drawHeader();
  } else {
    Serial.println("WiFi再接続失敗。少し待ってから再試行。");
    delay(5000);
  }
}

// BtnB 長押し (2 秒) で設定ポータルへ
void checkPortalButton() {
  static unsigned long pressedAt = 0;
  if (M5.BtnB.isPressed()) {
    if (pressedAt == 0) pressedAt = millis();
    if (millis() - pressedAt > 2000) {
      // ボタン離されるまで待ち、現セッションを畳んでポータルへ
      while (M5.BtnB.isPressed()) { M5.update(); delay(20); }
      ntrip_c.stop();
      ntripConnected = false;
      runConfigPortal();
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(200);
      }
      drawCurrentScreen();
      pressedAt = 0;
    }
  } else {
    pressedAt = 0;
  }
}

// ポータル中の LCD: AP情報 + WiFi-join QR + URL
void drawPortalScreen() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("CONFIG MODE");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 24);
  M5.Display.printf("SSID: %s\n", g_portalSsid.c_str());
  M5.Display.printf("PASS: %s\n", PORTAL_PASSWORD);
  M5.Display.println("URL : http://192.168.4.1/");
  M5.Display.println("");
  M5.Display.println("Scan QR to join WiFi");

  // WiFi-join QR (標準形式): スキャンするとスマホが自動でAPに接続する
  String wifiQr = "WIFI:T:WPA;S:" + g_portalSsid + ";P:" + String(PORTAL_PASSWORD) + ";;";
  // 画面右側に配置: 140x140 程度
  M5.Display.qrcode(wifiQr.c_str(), 170, 80, 140, 6);
}

// BtnC 短押しでリリース情報→必要なら OTA フロー
void checkReleaseButton() {
  if (M5.BtnC.wasClicked()) {
    showReleaseInfo();  // OTA成功時はここで再起動して戻ってこない
    drawCurrentScreen();
    lastTractorX = -TRACTOR_W;
  }
}

// ms ミリ秒経過 or 任意ボタンクリックで戻る
static void waitForDismiss(uint32_t ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    M5.update();
    if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnC.wasClicked()) break;
    delay(20);
  }
}

// BtnA で No、BtnC で Yes を返す
static bool waitForYesNo() {
  while (true) {
    M5.update();
    if (M5.BtnA.wasClicked()) return false;
    if (M5.BtnC.wasClicked()) return true;
    delay(20);
  }
}

// 「RELEASE INFO」ヘッダだけ描く共通部
static void drawReleaseHeader() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("RELEASE INFO");
}

// HTTPUpdate のダウンロード進捗を画面下部にバー描画
static void otaProgressCb(int cur, int total) {
  static int lastPct = -1;
  if (total <= 0) return;
  int pct = (int)((int64_t)cur * 100 / total);
  if (pct == lastPct) return;
  lastPct = pct;

  int w = M5.Display.width();
  int barX = 10, barY = 180, barW = w - 20, barH = 24;
  M5.Display.drawRect(barX, barY, barW, barH, WHITE);
  int fillW = (barW - 4) * pct / 100;
  M5.Display.fillRect(barX + 2, barY + 2, fillW, barH - 4, GREEN);

  M5.Display.setTextSize(2);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 145);
  M5.Display.printf("OTA: %3d%%  %d/%d KB    ",
                    pct, cur / 1024, total / 1024);
  Serial.printf("[OTA] %d%% %d/%d\n", pct, cur, total);
}

// HTTPUpdate.update() を実行する。成功したら自動で再起動するので戻ってこない。
// 失敗したら画面にエラーを出して戻る。
static void runOTA(const GitHubReleaseInfo& r) {
  String url = "https://github.com/" + String(GH_REPO) +
               "/releases/download/" + r.tagName + "/" +
               String(APP_NAME) + "-" + r.tagName + ".bin";

  Serial.printf("[OTA] Updating from %s\n", url.c_str());

  drawReleaseHeader();
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 30);
  M5.Display.printf("Updating to %s\n", r.tagName.c_str());
  M5.Display.println("Downloading firmware.bin...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  httpUpdate.rebootOnUpdate(true);   // 成功時は ESP.restart() を勝手に呼ぶ
  httpUpdate.setLedPin(-1);
  httpUpdate.onProgress(otaProgressCb);

  // GitHub Releases は objects.githubusercontent.com にリダイレクトする
  HTTPUpdateResult ret = httpUpdate.update(client, url, FIRMWARE_VERSION);

  drawReleaseHeader();
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(RED, BLACK);
  M5.Display.setCursor(0, 70);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      M5.Display.println("OTA FAILED");
      M5.Display.setTextSize(1);
      M5.Display.printf("err %d: %s\n",
                        httpUpdate.getLastError(),
                        httpUpdate.getLastErrorString().c_str());
      Serial.printf("[OTA] FAILED %d: %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      M5.Display.setTextColor(ORANGE, BLACK);
      M5.Display.println("NO UPDATE");
      Serial.println("[OTA] No update applied (server says no)");
      break;
    case HTTP_UPDATE_OK:
      // rebootOnUpdate(true) で再起動済みなのでここには来ない
      M5.Display.setTextColor(GREEN, BLACK);
      M5.Display.println("OTA OK");
      break;
  }
  waitForDismiss(8000);
}

// メイン: BtnC 短押しから呼ばれる。GitHub に問い合わせ、状況によって
// 「Up to date」「エラー」「Update available + リリースノート + Y/N」を分岐。
// Yes 選択時に runOTA() に進んで成功すれば再起動して戻らない。
void showReleaseInfo() {
  drawReleaseHeader();
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 30);
  M5.Display.printf("Current: %s\n", FIRMWARE_VERSION);
  M5.Display.println("");
  M5.Display.println("Querying GitHub...");

  GitHubReleaseInfo r = GitHubRelease::fetchLatest(GH_REPO);

  if (!r.ok) {
    M5.Display.setTextColor(RED, BLACK);
    M5.Display.printf("Failed: HTTP %d\n", r.httpCode);
    M5.Display.printf("(%s)\n", r.error.c_str());
    waitForDismiss(5000);
    return;
  }

  bool sameVersion = (String(FIRMWARE_VERSION) == r.tagName);
  if (sameVersion) {
    drawReleaseHeader();
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setCursor(0, 30);
    M5.Display.printf("Current: %s\n", FIRMWARE_VERSION);
    M5.Display.printf("Latest:  %s\n", r.tagName.c_str());
    M5.Display.printf("Date:    %s\n", r.publishedAt.substring(0, 10).c_str());
    M5.Display.println("");
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(GREEN, BLACK);
    M5.Display.println("Up to date");
    waitForDismiss(5000);
    return;
  }

  // ---- Update available: notes + Y/N prompt ----
  drawReleaseHeader();
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 30);
  M5.Display.printf("Current: %s -> %s\n", FIRMWARE_VERSION, r.tagName.c_str());
  M5.Display.printf("Date: %s\n", r.publishedAt.substring(0, 10).c_str());

  // 区切り
  M5.Display.println("--- release notes ---");

  // body は \n 入りなので println 連打。長くなりすぎないように切り詰める。
  // 表示できる行数: 残り y=70-180 = 110px / 8px ≒ 13 行
  int linesShown = 0;
  const int MAX_LINES = 13;
  String body = r.body;
  int start = 0;
  while (linesShown < MAX_LINES && start < (int)body.length()) {
    int eol = body.indexOf('\n', start);
    String line = (eol < 0) ? body.substring(start) : body.substring(start, eol);
    // 1 行 ~52 文字 (size 1, w=320, ~6px/ch)。安全側で 50。
    if (line.length() > 50) line = line.substring(0, 47) + "...";
    M5.Display.println(line);
    linesShown++;
    if (eol < 0) break;
    start = eol + 1;
  }
  if (start < (int)body.length()) {
    M5.Display.setTextColor(DARKGREY, BLACK);
    M5.Display.println("(truncated)");
  }

  // Y/N プロンプトを最下部に
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 188);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.println("Update? A=No  C=Yes");

  bool yes = waitForYesNo();
  if (yes) {
    runOTA(r);
  }
}

void runConfigPortal() {
#if NTRIP_CONFIG_PORTAL_AVAILABLE
  drawPortalScreen();

  Serial.println("====================================");
  Serial.printf("[Portal] Connect WiFi to:\n");
  Serial.printf("  SSID: %s\n", g_portalSsid.c_str());
  Serial.printf("  PASS: %s\n", PORTAL_PASSWORD);
  Serial.printf("  URL : http://192.168.4.1/\n");
  Serial.println("====================================");

  NTRIPConfigPortal portal;
  bool changed = portal.run(g_cfg, g_portalSsid, PORTAL_PASSWORD, PORTAL_TIMEOUT_MS);
  if (changed) {
    Serial.println("[Portal] 設定を保存しました。再起動します。");
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 100);
    M5.Display.setTextColor(GREEN, BLACK);
    M5.Display.println(" Saved. Rebooting...");
    delay(1500);
    ESP.restart();
  } else {
    Serial.println("[Portal] timeout / 変更なしで戻ります。");
  }
#else
  Serial.println("[Portal] not compiled in (missing ESPAsyncWebServer dep)");
#endif
}


// ---- 画面切替 ----------------------------------------------------------

// BtnB 短押しで画面トグル (長押しは checkPortalButton が拾う)
void checkScreenSwitchButton() {
  if (M5.BtnB.wasClicked()) {
    currentScreen = (currentScreen + 1) % 2;
    drawCurrentScreen();
    lastTractorX = -TRACTOR_W;  // 戻ったときに再描画されるよう
  }
}

// 現在の画面を全描画 (fillScreen + 内容 + ボタンラベル)
void drawCurrentScreen() {
  M5.Display.fillScreen(BLACK);
  if (currentScreen == 0) {
    drawHeader();
    drawStatus();
  } else {
    drawDeviceInfoScreen();
  }
  drawButtonLabels();
}

void drawDeviceInfoScreen() {
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("DEVICE INFO");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 24);

  M5.Display.println("[NTRIP]");
  M5.Display.printf("  host : %s:%u\n", g_cfg.host.c_str(), g_cfg.port);
  M5.Display.printf("  mnt  : %s\n",    g_cfg.mountpoint.c_str());
  M5.Display.printf("  user : %s\n",    g_cfg.user.length() ? g_cfg.user.c_str() : "(anon)");
  M5.Display.printf("  vrs  : %s (gga %us)\n",
                    g_cfg.vrsEnabled ? "ON" : "off",
                    (unsigned)g_cfg.ggaIntervalSec);

  M5.Display.println("[Network]");
  M5.Display.printf("  IP   : %s\n", WiFi.localIP().toString().c_str());
  M5.Display.printf("  MAC  : %s\n", WiFi.macAddress().c_str());

  uint64_t chipId = ESP.getEfuseMac();
  M5.Display.println("[Device]");
  M5.Display.printf("  chip : %04X%08X\n",
                    (uint16_t)(chipId >> 32), (uint32_t)chipId);
  M5.Display.printf("  fw   : %s\n", FIRMWARE_VERSION);
  M5.Display.printf("  app  : %s\n", APP_NAME);

  M5.Display.println("[UART]");
  M5.Display.printf("  PORT.A  RX=%d TX=%d  (RTK rx)\n", PORTA_RX_PIN, PORTA_TX_PIN);
  M5.Display.printf("  RS232F  RX=%d TX=%d  (NMEA out)\n", RS232F_RX_PIN, RS232F_TX_PIN);
}

// 画面下部 (y=212-228) にボタン A/B/C のラベル。BtnA は空白固定。
void drawButtonLabels() {
  int w = M5.Display.width();
  int third = w / 3;
  int yLabel = 212;

  M5.Display.fillRect(0, yLabel - 2, w, 18, BLACK);

  M5.Display.setTextSize(2);

  // BtnB 中央: "Display"
  const char* lblB = "Display";
  int lblBW = strlen(lblB) * 12;        // 12px/char (size 2)
  int xB    = third + (third - lblBW) / 2;
  M5.Display.setTextColor(CYAN, BLACK);
  M5.Display.setCursor(xB, yLabel);
  M5.Display.print(lblB);

  // BtnC 右: "Update"
  const char* lblC = "Update";
  int lblCW = strlen(lblC) * 12;
  int xC    = (third * 2) + (third - lblCW) / 2;
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setCursor(xC, yLabel);
  M5.Display.print(lblC);
}
