/*
 *  NTRIP <-> RTK受信機 ブリッジ版 (M5Atom)
 *
 *  M5AtomNTRIPClient.ino のブリッジ版。
 *  WiFi → NTRIP → Serial3(G26 TX) → 外部RTK受信機
 *  外部RTK受信機 → Serial3(G32 RX) → NMEA → Serial2(G22 TX)
 *
 *  ・WiFi接続はWiFiManager (tzapu/WiFiManager)
 *  ・指数バックオフ + ジッター付きで再接続(回線不安定対策)
 *  ・データストール(無音)検知で能動再接続
 *  ・LED は RTCM 受信レートに応じた色相シフトで生存表示
 *
 *  ピン配置 (M5Atom Grove ポートに合わせている):
 *    Serial2  TX: G22  RX: G19  (NMEA を外に出す)
 *    Serial3  TX: G26  RX: G32  (RTK受信機との通信)
 *
 *  ESP32 には Serial3 がないので UART1 を流用して Serial3 と命名している。
 *
 *  LEDの色:
 *    色相が回る  RTCM データ受信中(=NTRIP流量に比例して色相がシフト)
 *    赤          NTRIP接続試行中
 *    青          バックオフ待機中 / 設定ポータル中
 *    黄          WiFi切断中
 */
#define FASTLED_INTERNAL
#include <WiFi.h>
#include <WiFiManager.h>
#include "NTRIPClient.h"
#include "NTRIPConfig.h"
#include "NTRIPConfigPortal.h"
#include "M5Atom.h"

// ---- NTRIP 設定 ----
// 値は NVS から起動時にロード。空ならポータルを強制起動。
NTRIPConfig g_cfg;

// 起動時 / 長押し時に立てる設定ポータル AP の SSID / パスワード。
// SSID にはチップ ID を末尾4桁つけて個体を区別する。
String g_portalSsid;
const char* PORTAL_PASSWORD = "configme123";
const uint32_t PORTAL_TIMEOUT_MS = 5UL * 60UL * 1000UL;  // 5 分

NTRIPClient ntrip_c;
uint8_t DisBuff[2 + 5 * 5 * 3];

// ---- UART ----
// Serial2 (G22 TX / G19 RX): NMEA 出力
// Serial3 (G26 TX / G32 RX): RTK 受信機との通信 (UART1 を借りる)
HardwareSerial Serial3(1);

// ---- 再接続バックオフ設定 ----
const unsigned long INITIAL_BACKOFF_MS = 5UL * 1000UL;
const unsigned long MAX_BACKOFF_MS     = 10UL * 60UL * 1000UL;
const unsigned long DATA_TIMEOUT_MS    = 30UL * 1000UL;
const unsigned long WIFI_RETRY_MS      = 30UL * 1000UL;
const unsigned long REPORT_INTERVAL_MS = 5UL * 1000UL;

unsigned long lastDataTime         = 0;
unsigned long lastConnectAttempt   = 0;
unsigned long backoffMs            = 0;
int           consecutiveFailures  = 0;

// NTRIP 接続状態は自前で管理
bool          ntripConnected       = false;

// RTCM (NTRIP → 受信機) のスループット計測
unsigned long rtcmBytesTotal       = 0;
unsigned long rtcmBytesSinceReport = 0;
// NMEA (受信機 → Serial2) のスループット計測
unsigned long nmeaBytesTotal       = 0;
unsigned long nmeaBytesSinceReport = 0;
unsigned long lastReportTime       = 0;

// LED 色相シフト(RTCM 着信時のみ進む)
const unsigned long LED_UPDATE_MS     = 80UL;
const unsigned int  BYTES_PER_HUE_DEG = 500;   // 3500 B/s で約51秒で1周
const uint8_t       LED_BRIGHTNESS    = 64;
unsigned long       lastLedUpdate     = 0;
unsigned long       bytesSinceHue     = 0;
uint16_t            hueDeg            = 0;
// ---- 関数プロトタイプ (PlatformIO 用) ----
  void pumpNmeaToSerial2();
  void scheduleRetry(bool increase);
  void handleWifiDown();
  void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b);
  void setBuff(uint8_t Rdata, uint8_t Gdata, uint8_t Bdata);
  
// ---- 関数プロトタイプ (PlatformIO 用) ----
  void scheduleRetry(bool increase);
  void handleWifiDown();
  void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b);
  void setBuff(uint8_t Rdata, uint8_t Gdata, uint8_t Bdata);
  void runConfigPortal();
  void checkPortalButton();

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 22, 19); // NMEA を出す側
  Serial3.begin(115200, SERIAL_8N1, 26, 32); // RTK 受信機: RX=G26, TX=G32
  delay(10);
  M5.begin(true, false, true);
  setBuff(0x00, 0x00, 0x00);
  pinMode(0, OUTPUT);
  digitalWrite(0, LOW);

  // Chip ID 末尾でユニークな AP 名にする
  uint64_t chipId = ESP.getEfuseMac();
  char ssidBuf[24];
  snprintf(ssidBuf, sizeof(ssidBuf), "NTRIP-Bridge-%04X", (uint16_t)(chipId & 0xFFFF));
  g_portalSsid = ssidBuf;

  // ---- NTRIP 設定をロード ----
  g_cfg.load();

  // ---- WiFiManager ----
  setBuff(0x00, 0x00, 0x40); // 設定ポータル中:青
  M5.dis.displaybuff(DisBuff);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (!wm.autoConnect("M5Atom-NTRIP-Bridge")) {
    Serial.println("WiFi接続に失敗。再起動します。");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // NTRIP 設定が未保存ならポータルを起動 (WiFi STA を一度切って AP モード)
  if (!g_cfg.isComplete()) {
    Serial.println("NTRIP 設定が空です。設定ポータルを起動します。");
    runConfigPortal();
    // ポータル後は STA で再接続
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(200);
    }
  }

  randomSeed(esp_random());
  lastConnectAttempt = millis() - INITIAL_BACKOFF_MS;
  backoffMs = 0;

  setBuff(0x40, 0x00, 0x00); // 接続試行中:赤
  M5.dis.displaybuff(DisBuff);
}

void loop() {
  M5.update();

  // 物理ボタン (G39) の長押しで設定ポータルに突入
  checkPortalButton();

  // 受信機からの NMEA は NTRIP 状態に関係なく常に Serial2 へ流す
  pumpNmeaToSerial2();

  // 1) WiFi 状態チェック
  if (WiFi.status() != WL_CONNECTED) {
    ntripConnected = false;
    handleWifiDown();
    return;
  }

  // 2) NTRIP 接続中なら RTCM を Serial3 (RTK受信機) へ流す
  if (ntripConnected) {
    while (ntrip_c.available()) {
      char ch = ntrip_c.read();
      Serial3.write((uint8_t)ch);          // RTK 受信機へ
      lastDataTime = millis();
      rtcmBytesTotal++;
      rtcmBytesSinceReport++;
      bytesSinceHue++;
    }
    Serial3.flush();

    // LED 色相シフト
    unsigned long now = millis();
    if (now - lastLedUpdate >= LED_UPDATE_MS) {
      if (bytesSinceHue >= BYTES_PER_HUE_DEG) {
        uint16_t degAdvance = bytesSinceHue / BYTES_PER_HUE_DEG;
        bytesSinceHue      -= degAdvance * BYTES_PER_HUE_DEG;
        hueDeg = (hueDeg + degAdvance) % 360;
        uint8_t r, g, b;
        hsvToRgb(hueDeg, 255, LED_BRIGHTNESS, r, g, b);
        setBuff(r, g, b);
        M5.dis.displaybuff(DisBuff);
      }
      lastLedUpdate = now;
    }

    // スループット表示 (RTCM と NMEA 両方)
    if (now - lastReportTime >= REPORT_INTERVAL_MS) {
      unsigned long elapsed = now - lastReportTime;
      float rtcmBps = (float)rtcmBytesSinceReport * 1000.0f / (float)elapsed;
      float nmeaBps = (float)nmeaBytesSinceReport * 1000.0f / (float)elapsed;
      Serial.printf("[BRIDGE] %lums  RTCM in: %lu B (%.1f B/s, total %lu)  "
                    "NMEA out: %lu B (%.1f B/s, total %lu)\n",
                    elapsed,
                    rtcmBytesSinceReport, rtcmBps, rtcmBytesTotal,
                    nmeaBytesSinceReport, nmeaBps, nmeaBytesTotal);
      rtcmBytesSinceReport = 0;
      nmeaBytesSinceReport = 0;
      lastReportTime       = now;
    }

    // RTCM ストール検知
    if (millis() - lastDataTime > DATA_TIMEOUT_MS) {
      Serial.println("RTCM が一定時間来ません。NTRIP を切断して再接続します。");
      ntrip_c.stop();
      ntripConnected = false;
      scheduleRetry(false);
    }
    delay(5);
    return;
  }

  // 3) バックオフ待機中
  if (millis() - lastConnectAttempt < backoffMs) {
    setBuff(0x00, 0x00, 0x40);
    M5.dis.displaybuff(DisBuff);
    delay(200);
    return;
  }

  // 4) NTRIP 再接続を試みる
  setBuff(0x40, 0x00, 0x00);
  M5.dis.displaybuff(DisBuff);
  lastConnectAttempt = millis();

  Serial.printf("NTRIP接続試行 %s:%u/%s (連続失敗: %d回)\n",
                g_cfg.host.c_str(), g_cfg.port, g_cfg.mountpoint.c_str(),
                consecutiveFailures);
  ntrip_c.stop();
  delay(50);
  // NTRIPClient::reqRaw は legacy signature で char*/int& を受けるので変換用ローカルを使う
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
    bytesSinceHue        = 0;
    lastLedUpdate        = millis();
    hueDeg               = 120; // 緑からスタート
    setBuff(0x00, 0x40, 0x00);
    M5.dis.displaybuff(DisBuff);
  } else {
    Serial.println("NTRIP接続失敗");
    scheduleRetry(true);
  }
}

// Serial3 から NMEA を読み Serial2 へ転送
void pumpNmeaToSerial2() {
  while (Serial3.available()) {
    int b = Serial3.read();
    if (b < 0) break;
    Serial2.write((uint8_t)b);
    nmeaBytesTotal++;
    nmeaBytesSinceReport++;
  }
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
    long jitter = random(-jitterRange, jitterRange + 1);
    long withJitter = (long)backoffMs + jitter;
    if (withJitter < (long)INITIAL_BACKOFF_MS) withJitter = INITIAL_BACKOFF_MS;
    backoffMs = (unsigned long)withJitter;
  } else {
    if (backoffMs < INITIAL_BACKOFF_MS) backoffMs = INITIAL_BACKOFF_MS;
  }
  Serial.printf("次回試行まで %lu 秒待機\n", backoffMs / 1000UL);
  lastConnectAttempt = millis();
}

void handleWifiDown() {
  setBuff(0x40, 0x40, 0x00);
  M5.dis.displaybuff(DisBuff);
  Serial.println("WiFi切断中。再接続を待ちます。");

  ntrip_c.stop();
  WiFi.disconnect();
  delay(100);
  WiFi.reconnect();

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_RETRY_MS) {
    // 待機中も NMEA は流す
    pumpNmeaToSerial2();
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi再接続OK");
  } else {
    Serial.println("WiFi再接続失敗。少し待ってから再試行。");
    delay(5000);
  }
}

// HSV → RGB (h: 0-359, s/v: 0-255)
void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
  h %= 360;
  uint8_t  region    = h / 60;
  uint16_t remainder = ((uint16_t)(h % 60) * 255) / 60;
  uint8_t p = ((uint16_t)v * (255 - s)) / 255;
  uint8_t q = ((uint16_t)v * (255 - ((uint16_t)s * remainder / 255))) / 255;
  uint8_t t = ((uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255))) / 255;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

void setBuff(uint8_t Rdata, uint8_t Gdata, uint8_t Bdata) {
  // SK6812 は GRB 順
  DisBuff[0] = 0x05;
  DisBuff[1] = 0x05;
  for (int i = 0; i < 25; i++) {
    DisBuff[2 + i * 3 + 0] = Gdata;
    DisBuff[2 + i * 3 + 1] = Rdata;
    DisBuff[2 + i * 3 + 2] = Bdata;
  }
}

// G39 (M5.Btn) を 2 秒長押しで設定ポータルへ
void checkPortalButton() {
  static unsigned long pressedAt = 0;
  if (M5.Btn.isPressed()) {
    if (pressedAt == 0) pressedAt = millis();
    if (millis() - pressedAt > 2000) {
      // 確実にトリガーが認識されたことを示すため一度紫点灯
      setBuff(0x40, 0x00, 0x40);
      M5.dis.displaybuff(DisBuff);
      // ボタンが離されるまで待つ (再トリガー防止)
      while (M5.Btn.isPressed()) { M5.update(); delay(20); }
      // 接続中の NTRIP を畳んでポータルへ
      ntrip_c.stop();
      ntripConnected = false;
      runConfigPortal();
      // ポータル後は STA に戻して再接続
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(200);
      }
      pressedAt = 0;
    }
  } else {
    pressedAt = 0;
  }
}

void runConfigPortal() {
#if NTRIP_CONFIG_PORTAL_AVAILABLE
  // 設定中であることを LED で明示 (シアン点滅)
  setBuff(0x00, 0x40, 0x40);
  M5.dis.displaybuff(DisBuff);

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
    delay(1500);
    ESP.restart();
  } else {
    Serial.println("[Portal] timeout / 変更なしで戻ります。");
  }
#else
  Serial.println("[Portal] not compiled in (missing ESPAsyncWebServer dep)");
#endif
}