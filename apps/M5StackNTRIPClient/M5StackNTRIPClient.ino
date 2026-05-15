/*
 *  NTRIP Client for M5Stack Basic
 *
 *  M5AtomNTRIPClient の M5Stack 版。
 *  WiFi → NTRIP → Serial2(PORT.A) → 外部RTK受信機
 *
 *  ・PORT.A (G21/G22) を Serial2 に割り当て、RTCM3 を RTK 受信機に流す。
 *    Atom 版と同じ Grove ケーブルで接続できるよう、RX=21, TX=22 で揃えている。
 *  ・WiFi接続は WiFiManager (tzapu/WiFiManager)
 *    起動時に BtnA(A) を押しているとその場で設定ポータルに入る。
 *    そうでないときは autoConnect で過去設定接続、未設定なら AP が立ち上がる。
 *  ・指数バックオフ + ジッター + ストール検知付きで 4G 回線下でも安定動作。
 *    通常の失敗では ESP.restart() しない (loop() の中で淡々と再接続)。
 *
 *  LCD 表示:
 *    上段: NTRIP サーバ情報, IP
 *    中段: 受信総バイト + 状態 (OK / STALLED / BACKOFF / WIFI DOWN / CONNECTING)
 *    下段: 直近スループット (B/s)
 */
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <WiFiManager.h>
#include "NTRIPClient.h"
#include "GitHubRelease.h"

// CI が -DFIRMWARE_VERSION='"vX.Y.Z"' で上書きする。デフォルトは "dev"。
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

constexpr const char* GH_REPO  = "yasunorioi/NTRIP-client-for-Arduino";
constexpr const char* APP_NAME = "M5StackNTRIPClient";

// ---- NTRIP サーバ設定 ----
char* host     = "rtk.toiso.fit";
int   httpPort = 2101;
char* mntpnt   = "eniwa-bd982";
char* user     = "";
char* passwd   = "";

NTRIPClient ntrip_c;

// ---- UART ----
// PORT.A: RX=G21, TX=G22 (RTCM3 を RTK 受信機へ流す)
constexpr int PORTA_RX_PIN = 21;
constexpr int PORTA_TX_PIN = 22;
constexpr int UART_BPS     = 115200;

// ---- 再接続バックオフ設定 ----
const unsigned long INITIAL_BACKOFF_MS = 5UL * 1000UL;      // 初回失敗:5秒
const unsigned long MAX_BACKOFF_MS     = 10UL * 60UL * 1000UL; // 最大:10分
const unsigned long DATA_TIMEOUT_MS    = 30UL * 1000UL;     // 30秒データなし→切断扱い
const unsigned long WIFI_RETRY_MS      = 30UL * 1000UL;     // WiFi再接続待ち最大30秒
const unsigned long REPORT_INTERVAL_MS = 1UL * 1000UL;      // LCD/Serial 更新間隔

// ---- ランタイム状態 ----
enum class AppState : uint8_t { Connecting, Online, Backoff, Stalled, WifiDown };
AppState      state               = AppState::Connecting;

unsigned long lastDataTime        = 0;
unsigned long lastConnectAttempt  = 0;
unsigned long backoffMs           = 0;
int           consecutiveFailures = 0;
bool          ntripConnected      = false;

uint64_t      bytesTotal          = 0;
uint64_t      bytesSinceReport    = 0;
unsigned long lastReportTime      = 0;
float         lastBpsShown        = 0.0f;

// ---- トラクターアニメーション ----
// RTCM3 受信量に比例して画面下部をトラクターが左→右に進む。
// 画面最下部 (y=212-228) はボタンラベルのために空けたので、トラクターは
// その上 (y=190-208 程度) に。データが止まれば自然に静止する。
constexpr int TRACTOR_W           = 24;
constexpr int TRACTOR_H           = 16;
constexpr int TRACTOR_Y           = 190;
constexpr uint64_t BYTES_PER_PIXEL = 200;  // 200 B/px → 1KB/s で 5 px/秒

// ---- 画面切替 ----
// 0 = ステータス, 1 = デバイス情報, 2 = 消灯 (夜間作業時)
// BtnB 短押しで 0→1→2→0 とサイクル。消灯中は BtnC も画面復帰のみ。
volatile int currentScreen = 0;
constexpr uint8_t BRIGHTNESS_DEFAULT = 128;  // 0-255

int           lastTractorX     = -TRACTOR_W;
unsigned long lastTractorTick  = 0;

// ---- プロトタイプ ----
void setupWiFi();
void drawHeader();
void drawStatus();
void drawTractor(int x, int y);
void updateTractor(uint64_t totalBytes);
void scheduleRetry(bool increase);
void handleWifiDown();
const char* stateLabel(AppState s);
uint16_t stateColor(AppState s);
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

  M5.Display.setRotation(1);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);
  M5.Display.println("M5Stack NTRIP Client");

  setupWiFi();

  randomSeed(esp_random());
  // 起動直後に1回目を試行できるように
  lastConnectAttempt = millis() - INITIAL_BACKOFF_MS;
  backoffMs          = 0;
  state              = AppState::Connecting;

  drawCurrentScreen();
  lastReportTime = millis();
}

void loop() {
  M5.update();

  // BtnB 短押し=画面切替 / BtnC 短押し=Update
  checkScreenSwitchButton();
  checkReleaseButton();

  // 1) WiFi 状態チェック
  if (WiFi.status() != WL_CONNECTED) {
    ntripConnected = false;
    state          = AppState::WifiDown;
    drawStatus();
    handleWifiDown();
    return;
  }

  // 2) NTRIP 接続中ならデータ転送
  if (ntripConnected) {
    while (ntrip_c.available()) {
      char ch = ntrip_c.read();
      Serial2.print(ch);
      lastDataTime = millis();
      bytesTotal++;
      bytesSinceReport++;
    }
    Serial2.flush();
    updateTractor(bytesTotal);

    unsigned long now = millis();

    // 定期表示
    if (now - lastReportTime >= REPORT_INTERVAL_MS) {
      unsigned long elapsed = now - lastReportTime;
      lastBpsShown = (float)bytesSinceReport * 1000.0f / (float)elapsed;
      Serial.printf("[NTRIP] %lums: %llu B  total %llu  rate %.1f B/s\n",
                    elapsed, (unsigned long long)bytesSinceReport,
                    (unsigned long long)bytesTotal, lastBpsShown);
      bytesSinceReport = 0;
      lastReportTime   = now;

      state = (now - lastDataTime > 5000) ? AppState::Stalled : AppState::Online;
      drawStatus();
    }

    // データストール検知 → 切断 → 再接続
    if (now - lastDataTime > DATA_TIMEOUT_MS) {
      Serial.println("データ無音。NTRIP を切断して再接続。");
      ntrip_c.stop();
      ntripConnected = false;
      scheduleRetry(false); // ストール起因はカウンタ増やさない
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
  Serial.printf("NTRIP接続試行 (連続失敗: %d回)\n", consecutiveFailures);

  ntrip_c.stop();
  delay(50);
  if (ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd)) {
    Serial.println("NTRIP接続成功");
    lastDataTime        = millis();
    consecutiveFailures = 0;
    backoffMs           = 0;
    ntripConnected      = true;
    bytesSinceReport    = 0;
    lastReportTime      = millis();
    state               = AppState::Online;
    drawStatus();
  } else {
    Serial.println("NTRIP接続失敗");
    scheduleRetry(true);
    state = AppState::Backoff;
    drawStatus();
  }
}

// WiFiManager のポータル AP が立った瞬間に呼ばれる。LCD に WiFi-join QR
// を出してスマホからすぐ参加できるようにする。
void drawWifiManagerQr(const char* ssid) {
  M5.Display.setBrightness(BRIGHTNESS_DEFAULT);  // 消灯中でも復帰
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

  // BtnA を 2 秒間チェック
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
  wm.setAPCallback([](WiFiManager* /*mgr*/) { drawWifiManagerQr("NTRIP-Client"); });

  M5.Display.println("");
  if (doManualConfig) {
    wm.startConfigPortal("NTRIP-Client");
  } else {
    M5.Display.println("Connecting WiFi...");
    if (!wm.autoConnect("NTRIP-Client")) {
      Serial.println("WiFi接続失敗。設定ポータルへ。");
      // autoConnect 失敗時はポータルが立ち上がるので待つ
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
  M5.Display.printf("ntrip://%s:%d/%s\n", host, httpPort, mntpnt);
  M5.Display.printf("IP: %s  PORT.A %d bps\n",
                    WiFi.localIP().toString().c_str(), UART_BPS);
}

void drawStatus() {
  if (currentScreen != 0) return;
  // 中段: 状態ラベル (色付き) + バイト総数
  M5.Display.setTextSize(3);
  M5.Display.setCursor(0, 40);
  M5.Display.setTextColor(stateColor(state), BLACK);
  M5.Display.printf("%-10s", stateLabel(state));

  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 80);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.printf("Total: %llu B    ", (unsigned long long)bytesTotal);

  // 下段: スループット
  M5.Display.setCursor(0, 110);
  M5.Display.printf("Rate : %.1f B/s    ", lastBpsShown);

  // 失敗カウンタ・バックオフ
  M5.Display.setCursor(0, 140);
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
  M5.Display.fillRect(x,      y + 8, 24, 5, YELLOW);     // 車体
  M5.Display.fillRect(x + 10, y,      8, 8, YELLOW);     // キャブ
  M5.Display.drawRect(x + 10, y,      8, 8, DARKGREY);   // キャブ枠
  M5.Display.fillRect(x + 8,  y + 1,  2, 5, DARKGREY);   // 排気煙突
  M5.Display.fillCircle(x + 19, y + 14, 3, DARKGREY);    // 後輪 (大)
  M5.Display.fillCircle(x + 19, y + 14, 1, BLACK);
  M5.Display.fillCircle(x + 5,  y + 14, 2, DARKGREY);    // 前輪 (小)
}

void updateTractor(uint64_t totalBytes) {
  if (currentScreen != 0) return;
  unsigned long now = millis();
  if (now - lastTractorTick < 50) return;  // 最大 20 fps
  lastTractorTick = now;

  int w = M5.Display.width();
  int x = (int)((totalBytes / BYTES_PER_PIXEL) % (uint64_t)w);
  if (x == lastTractorX) return;

  // 最下部 1 ライン分まとめてクリアして描き直す
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
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi再接続OK");
    drawHeader(); // IP が変わっている可能性
  } else {
    Serial.println("WiFi再接続失敗。少し待ってから再試行。");
    delay(5000);
  }
}

// BtnC 短押しでリリース情報→必要なら OTA フロー
// 消灯モード中は OTA を起動せず画面復帰のみ
void checkReleaseButton() {
  if (M5.BtnC.wasClicked()) {
    if (currentScreen == 2) {
      currentScreen = 0;
      drawCurrentScreen();
      return;
    }
    showReleaseInfo();  // OTA成功時はここで再起動して戻ってこない
    drawCurrentScreen();
    lastTractorX = -TRACTOR_W;
  }
}

// ---- helpers ----
static void waitForDismiss(uint32_t ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    M5.update();
    if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnC.wasClicked()) break;
    delay(20);
  }
}

static bool waitForYesNo() {
  while (true) {
    M5.update();
    if (M5.BtnA.wasClicked()) return false;
    if (M5.BtnC.wasClicked()) return true;
    delay(20);
  }
}

static void drawReleaseHeader() {
  M5.Display.setBrightness(BRIGHTNESS_DEFAULT);  // 消灯中でも復帰
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.println("RELEASE INFO");
}

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

  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setLedPin(-1);
  httpUpdate.onProgress(otaProgressCb);

  HTTPUpdateResult ret = httpUpdate.update(client, url, FIRMWARE_VERSION);

  drawReleaseHeader();
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 70);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      M5.Display.setTextColor(RED, BLACK);
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
      break;
    case HTTP_UPDATE_OK:
      M5.Display.setTextColor(GREEN, BLACK);
      M5.Display.println("OTA OK");
      break;
  }
  waitForDismiss(8000);
}

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

  // Update available: release notes + Y/N
  drawReleaseHeader();
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setCursor(0, 30);
  M5.Display.printf("Current: %s -> %s\n", FIRMWARE_VERSION, r.tagName.c_str());
  M5.Display.printf("Date: %s\n", r.publishedAt.substring(0, 10).c_str());
  M5.Display.println("--- release notes ---");

  int linesShown = 0;
  const int MAX_LINES = 13;
  String body = r.body;
  int start = 0;
  while (linesShown < MAX_LINES && start < (int)body.length()) {
    int eol = body.indexOf('\n', start);
    String line = (eol < 0) ? body.substring(start) : body.substring(start, eol);
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

  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 188);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.println("Update? A=No  C=Yes");

  bool yes = waitForYesNo();
  if (yes) {
    runOTA(r);
  }
}

// ---- 画面切替 ----------------------------------------------------------

void checkScreenSwitchButton() {
  if (M5.BtnB.wasClicked()) {
    currentScreen = (currentScreen + 1) % 3;
    drawCurrentScreen();
    lastTractorX = -TRACTOR_W;
  }
}

// 消灯モード(2)では brightness 0 + 黒塗りで完全に暗くする
void drawCurrentScreen() {
  if (currentScreen == 2) {
    M5.Display.fillScreen(BLACK);
    M5.Display.setBrightness(0);
    return;
  }
  M5.Display.setBrightness(BRIGHTNESS_DEFAULT);
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

  M5.Display.println("[NTRIP] (hardcoded)");
  M5.Display.printf("  host : %s:%d\n", host, httpPort);
  M5.Display.printf("  mnt  : %s\n",    mntpnt);
  M5.Display.printf("  user : %s\n",    strlen(user) ? user : "(anon)");

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
  M5.Display.printf("  PORT.A  RX=%d TX=%d  %d bps\n",
                    PORTA_RX_PIN, PORTA_TX_PIN, UART_BPS);
}

void drawButtonLabels() {
  int w = M5.Display.width();
  int third = w / 3;
  int yLabel = 212;

  M5.Display.fillRect(0, yLabel - 2, w, 18, BLACK);
  M5.Display.setTextSize(2);

  const char* lblB = "Display";
  int lblBW = strlen(lblB) * 12;
  int xB = third + (third - lblBW) / 2;
  M5.Display.setTextColor(CYAN, BLACK);
  M5.Display.setCursor(xB, yLabel);
  M5.Display.print(lblB);

  const char* lblC = "Update";
  int lblCW = strlen(lblC) * 12;
  int xC = (third * 2) + (third - lblCW) / 2;
  M5.Display.setTextColor(YELLOW, BLACK);
  M5.Display.setCursor(xC, yLabel);
  M5.Display.print(lblC);
}
