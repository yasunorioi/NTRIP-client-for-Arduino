/*
 *  NTRIP client for M5Stack (M5Unified + WPS)
 *  - Connects to NTRIP caster and forwards RTCM data to Serial2
 *  - LCD status display with byte counter and bps
 *  - Uses WPS for WiFi setup
 */
#include <M5Unified.h>
#include <WiFi.h>
#include "esp_wps.h"
#include "wpsConnector.h"
#include "NTRIPClient.h"

NTRIPClient ntrip_c;

// ---- NTRIP Server Config ----
char* host     = "rtk.toiso.fit";
int   httpPort = 2101;
char* mntpnt   = "eniwa-bd982";
char* user     = "";
char* passwd   = "";

// ---- State ----
uint64_t totalBytes = 0;
uint64_t lastBytes  = 0;
unsigned long lastDataTime = 0;
const unsigned long STALL_TIMEOUT_MS = 5000;
int uart_bps = 115200;

// bps counter
hw_timer_t* timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile unsigned long countPerSec = 0;
int displayBps = 0;

void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  displayBps = countPerSec * 8;
  countPerSec = 0;
  portEXIT_CRITICAL_ISR(&timerMux);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.begin(uart_bps, SERIAL_8N1, 16, 17);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);
  M5.Display.println("WiFi WPS connecting...");

  WiFi.begin();
  int wificount = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wificount++;
    if (wificount == 5) {
      Serial.println("Starting WPS");
      WiFi.disconnect();
      wpsConnect();
    }
  }
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());
  M5.Display.println("WiFi connected!");
  configTime(9 * 3600, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");

  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.printf("ntrip://%s:%d/%s\n", host, httpPort, mntpnt);

  Serial.printf("Connecting: %s:%d/%s\n", host, httpPort, mntpnt);

  if (!ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd)) {
    Serial.println("NTRIP connection failed");
    M5.Display.println("NTRIP FAILED");
    delay(10000);
    ESP.restart();
  }

  Serial.println("NTRIP connected!");
  M5.Display.println("NTRIP connected!");
  lastDataTime = millis();

  timer = timerBegin(0, getApbFrequency() / 1000000, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 1000000, true);
  timerAlarmEnable(timer);
}

void loop() {
  M5.update();

  while (ntrip_c.available()) {
    char ch = ntrip_c.read();
    Serial2.print(ch);
    totalBytes++;
    portENTER_CRITICAL(&timerMux);
    countPerSec++;
    portEXIT_CRITICAL(&timerMux);
  }
  Serial2.flush();

  unsigned long now = millis();

  if (totalBytes > lastBytes) {
    lastBytes = totalBytes;
    lastDataTime = now;
  }

  bool stalled = (now - lastDataTime > STALL_TIMEOUT_MS);

  if (!ntrip_c.connected()) {
    Serial.println("NTRIP disconnected, restarting...");
    M5.Display.println("\nDISCONNECTED");
    delay(5000);
    ESP.restart();
  }

  if (stalled && (now - lastDataTime > 30000)) {
    Serial.println("Stalled 30s, restarting...");
    ntrip_c.stop();
    delay(1000);
    ESP.restart();
  }

  // Update display
  static unsigned long lastPrint = 0;
  if (now - lastPrint >= 1000) {
    M5.Display.setTextSize(4);
    M5.Display.setCursor(0, 48);
    M5.Display.printf("%d    ", displayBps);
    M5.Display.setTextSize(2);
    M5.Display.print("bps");

    M5.Display.setTextSize(1);
    M5.Display.setCursor(0, 100);
    M5.Display.printf("Total: %llu bytes  ", totalBytes);
    M5.Display.setCursor(0, 112);
    M5.Display.printf("Status: %s    ", stalled ? "STALLED!" : "OK");

    M5.Display.setCursor(0, 224);
    M5.Display.setTextSize(2);
    M5.Display.printf("RS232c: %dbps", uart_bps);

    lastPrint = now;
  }

  delay(10);
}
