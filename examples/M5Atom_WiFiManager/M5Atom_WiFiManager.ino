/*
 *  NTRIP client for M5Atom (M5Unified + WiFiManager)
 *  - Connects to NTRIP caster and forwards RTCM data to Serial2
 *  - LED: Green=receiving, Red=disconnected, Rainbow=stalled
 *  - Hold button at boot to enter WiFi config portal
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
const unsigned long STALL_TIMEOUT_MS = 5000; // 5 sec no data = stalled
uint8_t rainbowHue = 0;

// HSV to RGB (hue: 0-255, sat/val: 0-255)
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
  M5.dis.drawpix(0, (CRGB){r, g, b});
}

void setLedRainbow() {
  uint8_t r, g, b;
  hsvToRgb(rainbowHue, 255, 64, r, g, b);
  setLed(r, g, b);
  rainbowHue += 4;
}

// ---- WiFi Setup ----
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
    setLed(0, 0, 0x40); // Blue = config mode
    wm.startConfigPortal("NTRIP-Client");
  } else {
    Serial.println("WiFi connecting...");
    setLed(0x40, 0x40, 0); // Yellow = connecting
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

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 22, 19);

  setLed(0x40, 0, 0); // Red = starting
  setupWiFi();

  Serial.print("Connecting to NTRIP: ");
  Serial.print(host);
  Serial.print(":");
  Serial.print(httpPort);
  Serial.print("/");
  Serial.println(mntpnt);

  if (!ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd)) {
    Serial.println("NTRIP connection failed, restarting...");
    setLed(0x40, 0, 0);
    delay(15000);
    ESP.restart();
  }

  Serial.println("NTRIP connected!");
  setLed(0, 0x40, 0); // Green = connected
  lastDataTime = millis();
}

void loop() {
  M5.update();

  // Read RTCM data
  while (ntrip_c.available()) {
    char ch = ntrip_c.read();
    Serial2.print(ch);
    totalBytes++;
  }

  unsigned long now = millis();

  // Check for new data
  if (totalBytes > lastBytes) {
    lastBytes = totalBytes;
    lastDataTime = now;
    setLed(0, 0x40, 0); // Green = receiving
  }

  // Stall detection: no new data for STALL_TIMEOUT_MS
  if (now - lastDataTime > STALL_TIMEOUT_MS) {
    setLedRainbow(); // Rainbow = stalled
    // If stalled too long, try reconnecting
    if (now - lastDataTime > 30000) {
      Serial.println("Stalled 30s, restarting...");
      ntrip_c.stop();
      delay(1000);
      ESP.restart();
    }
  }

  // Connection lost
  if (!ntrip_c.connected()) {
    Serial.println("NTRIP disconnected, restarting...");
    setLed(0x40, 0, 0); // Red
    delay(5000);
    ESP.restart();
  }

  // Print stats every second
  static unsigned long lastPrint = 0;
  if (now - lastPrint >= 1000) {
    Serial.printf("RTCM bytes: %llu\n", totalBytes);
    lastPrint = now;
  }

  delay(10);
}
