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
#include <WiFiManager.h>
#include "NTRIPClient.h"

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
// RTCM3 受信量に比例して画面最下部をトラクターが左→右に進む。
// データが止まれば totalBytes が増えないので自然に静止する。
constexpr int TRACTOR_W           = 24;
constexpr int TRACTOR_H           = 16;
constexpr int TRACTOR_Y           = 220;
constexpr uint64_t BYTES_PER_PIXEL = 200;  // 200 B/px → 1KB/s で 5 px/秒

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

  M5.Display.fillScreen(BLACK);
  drawHeader();
  drawStatus();
  lastReportTime = millis();
}

void loop() {
  M5.update();

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
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.printf("ntrip://%s:%d/%s\n", host, httpPort, mntpnt);
  M5.Display.printf("IP: %s  PORT.A %d bps\n",
                    WiFi.localIP().toString().c_str(), UART_BPS);
}

void drawStatus() {
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
