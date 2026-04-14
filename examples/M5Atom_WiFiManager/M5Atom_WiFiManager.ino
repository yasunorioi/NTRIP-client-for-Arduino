#include <M5Atom.h>
#include "NTRIPClient.h"
#include <WiFiManager.h>
WiFiManager wifiManager;
NTRIPClient ntrip_c;

char* host     = "rtk.toiso.fit";
int   httpPort = 2101;
char* mntpnt   = "eniwa-bd982";
char* user     = "";
char* passwd   = "";

bool isWifiConfigSucceeded = false;

uint8_t DisBuff[2 + 5 * 5 * 3];
uint64_t Count;
uint8_t WiFiCount;
uint8_t WiFiStatus;

// WiFi接続モードに移行した時に呼ばれるコールバック
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

// 起動後すぐにパワーボタンが押されたらWiFi設定モードに移行し、そうでなければ自動接続を行う
void setupWiFi()
{
  wifiManager.setAPCallback(configModeCallback);

  // clicking power button at boot time to enter wifi config mode
  bool doManualConfig = false;
  Serial.println("Push button to enter Wifi config.");
  for(int i=0 ; i<200 ; i++) {
    M5.update();
    if (M5.Btn.isPressed()) {
      doManualConfig = true;
      break;
    }
    delay(10);
  }

  if (doManualConfig) {
    Serial.println("wifiManager.startConfigPortal()");
    if (wifiManager.startConfigPortal()) {
      isWifiConfigSucceeded = true;
      Serial.println("startConfigPortal() connect success!");
    }
    else {
      Serial.println("startConfigPortal() connect failed!");
    }
  } else {
    Serial.println("Wi-Fi connecting...");

    Serial.println("wifiManager.autoConnect()");
    if (wifiManager.autoConnect()) {
      isWifiConfigSucceeded = true;
      Serial.println("autoConnect() connect success!");
    }
    else {
      Serial.println("autoConnect() connect failed!");
    }
  }

  if (isWifiConfigSucceeded) {
    Serial.println("Wi-Fi connected.");
  } else {
    Serial.println("Wi-Fi failed.");
  }
}



void setBuff(uint8_t Rdata, uint8_t Gdata, uint8_t Bdata)
{
    DisBuff[0] = 0x05;
    DisBuff[1] = 0x05;
    for (int i = 0; i < 25; i++)
    {
        DisBuff[2 + i * 3 + 0] = Rdata;
        DisBuff[2 + i * 3 + 1] = Gdata;
        DisBuff[2 + i * 3 + 2] = Bdata;
    }
}

void setup() {
  M5.begin(true, false, true);
  Serial.begin(115200);
  Serial2.begin(115200,SERIAL_8N1,22,19);
  setupWiFi();
  Serial.println("M5Atom started.");

  setBuff(0x40, 0x00, 0x00);
  M5.dis.displaybuff(DisBuff);

  if(!ntrip_c.reqRaw(host,httpPort,mntpnt,user,passwd)){
    delay(15000);
    ESP.restart();
  }
  Serial.print("mntpnt:");
  Serial.println(mntpnt);
}

void loop() {
  while(ntrip_c.available()) {
    char ch = ntrip_c.read();
    Serial2.print(ch);
    Count++;
  }
  if (Count>0){
    Serial.println(Count);
  }
  delay(1000);
  M5.update();
}
