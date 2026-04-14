/*
 *  NTRIP client for M5Stack (M5Unified + WiFiManager)
 *  - Connects to NTRIP caster and forwards RTCM data to Serial2
 *  - LCD status display with byte counter
 *  - Hold BtnA at boot to enter WiFi config portal
 */
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
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

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.begin(uart_bps, SERIAL_8N1, 16, 17);

  setupWiFi();

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
}

void loop() {
  M5.update();

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

  // Update display every second
  static unsigned long lastPrint = 0;
  if (now - lastPrint >= 1000) {
    M5.Display.setTextSize(3);
    M5.Display.setCursor(0, 48);
    M5.Display.printf("%llu  ", totalBytes);

    M5.Display.setTextSize(1);
    M5.Display.setCursor(0, 90);
    M5.Display.printf("Status: %s    ", stalled ? "STALLED!" : "OK");

    M5.Display.setCursor(0, 224);
    M5.Display.setTextSize(2);
    M5.Display.printf("RS232c: %dbps", uart_bps);

    Serial.printf("RTCM bytes: %llu%s\n", totalBytes, stalled ? " [STALLED]" : "");
    lastPrint = now;
  }

  delay(10);
}
