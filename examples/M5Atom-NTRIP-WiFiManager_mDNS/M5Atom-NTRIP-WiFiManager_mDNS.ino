#include <M5Atom.h>
#include "NTRIPClient.h"
#include <WiFiManager.h>
#include <ESPmDNS.h>
WiFiManager wifiManager;
NTRIPClient ntrip_c;

//char* host = "rtk.toiso.fit";
int httpPort = 2101; //port 2101 is default port of NTRIP caster
char* mntpnt = "eniwa-bd982";
char* user   = "";
char* passwd = "";

bool isWifiConfigSucceeded = false;

const char* MDNS_NAME="m5stack";

//ntrip server  .local不要
const char* server = "your NTRIP server mDNS";
// https://qiita.com/mhama/items/ff5ae397a853aa4f8d48

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

void setup() {
  M5.begin(true, false, true);
  Serial.begin(115200);
  Serial2.begin(115200,SERIAL_8N1,22,19);
  setupWiFi();
  Serial.println("M5Atom started.");
  if (!MDNS.begin(MDNS_NAME)){
    Serial.print("ERROR MDNS_NAME");
    ESP.restart();
  }
  IPAddress server_ip = MDNS.queryHost(server);
  char host[30];
  sprintf(host,"%u.%u.%u.%u",server_ip[0],server_ip[1],server_ip[2],server_ip[3]);
  Serial.print("Server:");
  Serial.println(host);
  if(!ntrip_c.reqRaw(host,httpPort,mntpnt,user,passwd)){
    //delay(15000);
    ESP.restart();
  }
}

void loop() {
  int count=0;
    while(ntrip_c.available()) {
      char ch = ntrip_c.read();        
      Serial2.print(ch);
      count++;
    }
  if (count>0){
    Serial.println(count);
  }
}
