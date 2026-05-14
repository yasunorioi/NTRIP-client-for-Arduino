/*
 *  Basic NTRIPClient usage
 *
 *  Connect to WiFi, then to an NTRIP caster, and print the received RTCM3
 *  bytes (typically ~1-5 KB/s) to USB Serial. No M5* hardware required.
 *
 *  For real applications (LCD/LED status, exponential backoff, NMEA bridge,
 *  hardware-specific UART output, etc.) see the apps/ folder.
 */
#include <WiFi.h>
#include "NTRIPClient.h"

// ---- WiFi ----
const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-password";

// ---- NTRIP caster ----
char* host    = "your.ntrip.host";   // e.g. "rtcm-ntrip.org"
int   port    = 2101;                // NTRIP default
char* mntpnt  = "YOUR_MOUNTPOINT";
char* user    = "";                  // empty if not required
char* passwd  = "";

NTRIPClient ntrip_c;

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.printf("Connecting to %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());

  Serial.printf("Connecting NTRIP %s:%d/%s ...\n", host, port, mntpnt);
  if (ntrip_c.reqRaw(host, port, mntpnt, user, passwd)) {
    Serial.println("NTRIP connected.");
  } else {
    Serial.println("NTRIP connection failed.");
  }
}

void loop() {
  while (ntrip_c.available()) {
    Serial.write(ntrip_c.read());  // raw RTCM3 to USB Serial
  }
}
