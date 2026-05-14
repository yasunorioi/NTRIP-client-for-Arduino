/*
 *  NTRIP client for Arduino Ver. 1.0.0
 *  NTRIPClient Sample
 *  Request Reference Data
 *
 *  WiFi接続はWiFiManagerを使用 (tzapu/WiFiManager)
 *  起動時に過去の接続情報がない場合や接続に失敗した場合、
 *  "M5Atom-NTRIP" というAPが立ち上がるので、スマホ等から接続して
 *  ブラウザでSSID/パスワードを設定してください。
 *
 *  4G回線が不安定な状況での連続再接続(=サーバから見るとDDoS)を防ぐため、
 *  - 指数バックオフ + ジッター
 *  - WiFi切断時はNTRIPリトライしない
 *  - データストール(無音)検知で能動的に再接続
 *  - ESP.restart() を排除し loop() 内で再接続
 *  を実装。
 *
 *  LEDの色:
 *    緑     データ受信中
 *    赤     NTRIP接続試行中
 *    青     バックオフ待機中 / 設定ポータル中
 *    黄     WiFi切断中
 */
#define FASTLED_INTERNAL
//#include <ESP8266WiFi.h>  //Need for ESP8266
#include <WiFi.h>           //Need for ESP32
#include <WiFiManager.h>    // https://github.com/tzapu/WiFiManager
#include "NTRIPClient.h"
#include "M5Atom.h"

char* host = "rtk.toiso.fit";
int httpPort = 2101; //port 2101 is default port of NTRIP caster
char* mntpnt = "eniwa-bd982";
char* user   = "";
char* passwd = "";
NTRIPClient ntrip_c;
uint8_t DisBuff[2 + 5 * 5 * 3];

// ---- 再接続バックオフ設定 ----
const unsigned long INITIAL_BACKOFF_MS = 5UL * 1000UL;      // 初回失敗時:5秒
const unsigned long MAX_BACKOFF_MS     = 10UL * 60UL * 1000UL; // 最大:10分
const unsigned long DATA_TIMEOUT_MS    = 30UL * 1000UL;     // 30秒データなし→切断とみなす
const unsigned long WIFI_RETRY_MS      = 30UL * 1000UL;     // WiFi再接続待ち最大30秒
const unsigned long REPORT_INTERVAL_MS = 5UL * 1000UL;      // スループット表示間隔

unsigned long lastDataTime      = 0;
unsigned long lastConnectAttempt = 0;
unsigned long backoffMs         = 0;
int           consecutiveFailures = 0;

// NTRIP 接続状態は ntrip_c.connected() に頼らず自前で管理
// (NTRIPClient 実装によっては接続後も connected() が false を返すことがある)
bool          ntripConnected    = false;
unsigned long bytesTotal        = 0;
unsigned long bytesSinceReport  = 0;
unsigned long lastReportTime    = 0;

// LED 色相シフト(データ着信時のみHueが進む。停止すれば色が固まる)
const unsigned long LED_UPDATE_MS      = 80UL;   // LED更新間隔
const unsigned int  BYTES_PER_HUE_DEG  = 200;    // 何バイトで1度進むか
                                                 // 3500 B/s なら約51秒で1周, 300 B/s なら約10分で1周
const uint8_t       LED_BRIGHTNESS     = 64;     // 0-255。眩しすぎないように
unsigned long       lastLedUpdate      = 0;
unsigned long       bytesSinceHue      = 0;
uint16_t            hueDeg             = 0;      // 0-359

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 22, 19);
  delay(10);
  M5.begin(true, false, true);
  setBuff(0x00, 0x00, 0x00);
  pinMode(0, OUTPUT);
  digitalWrite(0, LOW);

  // ---- WiFiManager で WiFi 接続 ----
  setBuff(0x00, 0x00, 0x40); // 設定ポータル中は青
  M5.dis.displaybuff(DisBuff);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  // WiFiが切れたときに自動再接続するように
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (!wm.autoConnect("M5Atom-NTRIP")) {
    Serial.println("WiFi接続に失敗。再起動します。");
    delay(3000);
    ESP.restart(); // 設定ポータル自体がタイムアウトした場合だけ再起動
  }

  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // ジッター用にシード初期化
  randomSeed(esp_random());

  // すぐ1回目を試行できるように
  lastConnectAttempt = millis() - INITIAL_BACKOFF_MS;
  backoffMs = 0;

  setBuff(0x40, 0x00, 0x00); // 接続試行中:赤
  M5.dis.displaybuff(DisBuff);
}

void loop() {
  M5.update();

  // 1) WiFi 状態チェック
  if (WiFi.status() != WL_CONNECTED) {
    ntripConnected = false;
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
      bytesSinceHue++;
    }
    Serial2.flush();

    // 色相シフト: 一定バイト届くたびにHueを1度進める。
    // データが止まればHueも止まる(=色が固まって停止を視認できる)。
    unsigned long now = millis();
    if (now - lastLedUpdate >= LED_UPDATE_MS) {
      if (bytesSinceHue >= BYTES_PER_HUE_DEG) {
        uint16_t degAdvance = bytesSinceHue / BYTES_PER_HUE_DEG;
        bytesSinceHue      -= degAdvance * BYTES_PER_HUE_DEG; // 端数は次回に持ち越し
        hueDeg = (hueDeg + degAdvance) % 360;
        uint8_t r, g, b;
        hsvToRgb(hueDeg, 255, LED_BRIGHTNESS, r, g, b);
        setBuff(r, g, b);
        M5.dis.displaybuff(DisBuff);
      }
      lastLedUpdate = now;
    }

    // 定期的にスループットを Serial へ表示
    if (now - lastReportTime >= REPORT_INTERVAL_MS) {
      float bps = (float)bytesSinceReport * 1000.0f / (float)(now - lastReportTime);
      Serial.printf("[NTRIP] last %lums: %lu B  total: %lu B  rate: %.1f B/s\n",
                    now - lastReportTime, bytesSinceReport, bytesTotal, bps);
      bytesSinceReport = 0;
      lastReportTime   = now;
    }

    // データストール検知
    if (millis() - lastDataTime > DATA_TIMEOUT_MS) {
      Serial.println("データが一定時間来ません。切断して再接続します。");
      ntrip_c.stop();
      ntripConnected = false;
      // バックオフはまだ増やさない(1回目の失敗扱いにする)
      scheduleRetry(false);
    }
    delay(5);
    return;
  }

  // 3) 切断中: バックオフが明けるまで待機
  if (millis() - lastConnectAttempt < backoffMs) {
    setBuff(0x00, 0x00, 0x40); // バックオフ中:青
    M5.dis.displaybuff(DisBuff);
    delay(200);
    return;
  }

  // 4) 再接続を試みる
  setBuff(0x40, 0x00, 0x00); // 試行中:赤
  M5.dis.displaybuff(DisBuff);
  lastConnectAttempt = millis();

  Serial.printf("NTRIP接続試行 (連続失敗: %d回)\n", consecutiveFailures);
  ntrip_c.stop();
  delay(50);
  if (ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd)) {
    Serial.println("NTRIP接続成功");
    lastDataTime       = millis();
    consecutiveFailures = 0;
    backoffMs          = 0;
    ntripConnected     = true;
    bytesSinceReport   = 0;
    lastReportTime     = millis();
    bytesSinceHue      = 0;
    lastLedUpdate      = millis();
    hueDeg             = 120; // 接続成功は緑(=Hue 120度)からスタート
    setBuff(0x00, 0x40, 0x00);
    M5.dis.displaybuff(DisBuff);
  } else {
    Serial.println("NTRIP接続失敗");
    scheduleRetry(true);
  }
}

// 次回リトライまでの待機時間を計算
//   increase=true なら失敗カウントを増やして指数バックオフを伸ばす
//   increase=false ならカウントを増やさず、すぐ1回はトライさせる
void scheduleRetry(bool increase) {
  if (increase) {
    consecutiveFailures++;
    if (backoffMs == 0) {
      backoffMs = INITIAL_BACKOFF_MS;
    } else {
      backoffMs *= 2;
      if (backoffMs > MAX_BACKOFF_MS) backoffMs = MAX_BACKOFF_MS;
    }
    // ±20% のジッターを乗せて他クライアントとの同期リトライを回避
    long jitterRange = (long)(backoffMs / 5);
    long jitter = random(-jitterRange, jitterRange + 1);
    long withJitter = (long)backoffMs + jitter;
    if (withJitter < (long)INITIAL_BACKOFF_MS) withJitter = INITIAL_BACKOFF_MS;
    backoffMs = (unsigned long)withJitter;
  } else {
    // ストール検知時は短めに(ただし最低INITIAL_BACKOFF_MSは置く)
    if (backoffMs < INITIAL_BACKOFF_MS) backoffMs = INITIAL_BACKOFF_MS;
  }
  Serial.printf("次回試行まで %lu 秒待機\n", backoffMs / 1000UL);
  lastConnectAttempt = millis();
}

// WiFi が切れているときの処理
void handleWifiDown() {
  setBuff(0x40, 0x40, 0x00); // 黄
  M5.dis.displaybuff(DisBuff);
  Serial.println("WiFi切断中。再接続を待ちます。");

  // NTRIPセッションも一度切る(切らないと半開きの恐れ)
  ntrip_c.stop();

  // 既存接続情報で再接続を試行
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
    // WiFiが復活した直後はNTRIPもすぐ試させる(ただしバックオフは尊重)
    // = 何もしない。loop()の通常フローに任せる。
  } else {
    Serial.println("WiFi再接続失敗。少し待ってから再試行。");
    // 全力リトライしないよう少し待つ
    delay(5000);
  }
}

// HSV → RGB 変換 (h: 0-359, s/v: 0-255)
void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
  h %= 360;
  uint8_t  region    = h / 60;
  uint16_t remainder = ((uint16_t)(h % 60) * 255) / 60; // 0-254
  uint8_t p = ((uint16_t)v * (255 - s)) / 255;
  uint8_t q = ((uint16_t)v * (255 - ((uint16_t)s * remainder / 255))) / 255;
  uint8_t t = ((uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255))) / 255;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break; // case 5
  }
}

void setBuff(uint8_t Rdata, uint8_t Gdata, uint8_t Bdata) {
  // M5Atom Matrix の LED は SK6812 で内部色順は GRB。
  // 呼び出し側を R,G,B のままにしておけるよう、ここでバイト順を入れ替える。
  DisBuff[0] = 0x05;
  DisBuff[1] = 0x05;
  for (int i = 0; i < 25; i++) {
    DisBuff[2 + i * 3 + 0] = Gdata;
    DisBuff[2 + i * 3 + 1] = Rdata;
    DisBuff[2 + i * 3 + 2] = Bdata;
  }
}
