#include <M5Atom.h>
#include "NTRIPClient.h"
#include <WiFiManager.h>
#include <EEPROM.h>
//#include <ESPmDNS.h>
WiFiManager wifiManager;
NTRIPClient ntrip_c;

//char* host = "rtk.toiso.fit";
/*
int httpPort = 2101; //port 2101 is default port of NTRIP caster
char* mntpnt = "eniwa-bd982";
char* user   = "";
char* passwd = "";
*/
bool isWifiConfigSucceeded = false;

const char* MDNS_NAME="m5stack";

//ntrip server  .local不要
const char* server = "your NTRIP server mDNS";
// https://qiita.com/mhama/items/ff5ae397a853aa4f8d48

uint8_t baseCount=1;
char* host[]={
  //Red
  "rtk.toiso.fit",
  //Green
  "rtk.toiso.fit",
  //Blue
  "117.102.192.33",
  //light blue
  "rtk.toiso.fit"
};
int httpPort[]={
  2101,
  2101,
  2101,
  2101
};
char* mntpnt[]={
  "eniwa-robo",
  "eniwa-f9p",
  "eniwa-kazui",
  "eniwa-bd970"
};
char* user[]={
  "",
  "",
  "",
  ""
};
char* passwd[]={
  "",
  "",
  "",
  ""
};
uint8_t DisBuff[2 + 5 * 5 * 3];
uint8_t ntripnum;
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
void eeprom_read(){
EEPROM.get(0,ntripnum);
if (ntripnum>=baseCount){
  EEPROM.put(0,0);
  EEPROM.commit();
  ntripnum=0;
 }
}

void setup() {
  M5.begin(true, false, true);
  Serial.begin(115200);
  Serial2.begin(115200,SERIAL_8N1,22,19);
  setupWiFi();
  Serial.println("M5Atom started.");
/*  if (!MDNS.begin(MDNS_NAME)){
    Serial.print("ERROR MDNS_NAME");
    ESP.restart();
  }*/
    EEPROM.begin(1);
    eeprom_read();
    switch (ntripnum)
    {
      case 0:
        //RED
        setBuff(0x40, 0x00, 0x00);
      break;
      case 1:
        //Green
        setBuff(0x00, 0x40, 0x00);
      break;
      case 2:
        //Blue
        setBuff(0x00, 0x00, 0x40);
      break;
      case 3:
        //pink
        setBuff(0x00, 0x40, 0x40);
      break;

      default:
      setBuff(0x00, 0x00, 0x00);
      break;

    }
    M5.dis.displaybuff(DisBuff);
    
 // IPAddress server_ip = MDNS.queryHost(server);
//  char host[30];
//  sprintf(host,"%u.%u.%u.%u",server_ip[0],server_ip[1],server_ip[2],server_ip[3]);
  if(!ntrip_c.reqRaw(host[ntripnum],httpPort[ntripnum],mntpnt[ntripnum],user[ntripnum],passwd[ntripnum])){
    //delay(15000);
    ntripnum++;
    EEPROM.put(0,ntripnum);
    EEPROM.commit();
    delay(1000);
    ESP.restart();
  }
  Serial.print("ntripnum change:");
  Serial.print(ntripnum);
  Serial.print(",mntpnt:");
  Serial.print(mntpnt[ntripnum]);
}

void loop() {
//  bool doNtripConfig=false;
    while(ntrip_c.available()) {
      char ch = ntrip_c.read();        
      Serial2.print(ch);
      Count++;
    }
  if (Count>0){
    Serial.println(Count);
  }
  if (Count>50000){
    setBuff(0x00, 0x00, 0x00);
    M5.dis.displaybuff(DisBuff);
  }
    Serial.println("Push button to NTRIP next hostnum.");
  for(int i=0 ; i<200 ; i++) {
    M5.update();
    if (M5.Btn.isPressed()) {
        ntripnum++;
        if (ntripnum >= baseCount)
          {
          ntripnum = 0;
          }
      EEPROM.put(0,ntripnum);
      EEPROM.commit();
      Serial.print("change ntripnum:");
      Serial.print(ntripnum);
      Serial.print(",mntpnt:");
      Serial.println(mntpnt[ntripnum]);
      delay(50);
      ESP.restart();
    }
    delay(10);
  }
}
