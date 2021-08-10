/*
author=yasunorioi
maintainer=yasunorioi  <bb.folf@gmail.com>
sentence=Library for M5Atomic RS232 with high precision reciever 
paragraph=
category=Communication
url=https://github.com/yasunorioi/NTRIP-client-for-Arduino
architectures=esp8266,esp32
*/

#include "M5Atom.h"
#include <EEPROM.h>
#include <WiFi.h> 
#include "NTRIPClient.h"
#include "esp_wifi.h"

const char* ssid     = "SSID";
const char* password = "pass";

uint8_t baseCount=4;
char* host[]={
  //Red
  "rtk.toiso.fit",
  //Green
  "rtk.toiso.fit",
  //Blue
  "117.102.192.33",
  //light blue
  "caster.agri-info-design.com"
};
int httpPort[]={
  2101,
  2101,
  2101,
  2101
};
char* mntpnt[]={
  "eniwa-bd982cmr",
  "eniwa-f9p",
  "eniwa-kazui",
  "00001122"
};
char* user[]={
  "",
  "",
  "",
  "USER"
};
char* passwd[]={
  "",
  "",
  "",
  "pass"
};

NTRIPClient ntrip_c;
uint8_t DisBuff[2 + 5 * 5 * 3];
uint8_t FSM;
uint64_t Count;
uint8_t WiFiCount;
uint8_t WiFiStatus;

void setup() {
    Serial.begin(115200);
    Serial2.begin(115200,SERIAL_8N1,22,19);
    M5.begin(true, false, true);
    delay(10);
    //setBuff(0xff, 0x00, 0x00);

    delay(10);
    Serial.println();
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
/*      WiFiCount++;
      Serial.print(".");
            if (WiFiCount == 5 || WiFiStatus == WL_NO_SSID_AVAIL) {
            esp_wifi_restore();
            WiFi.begin(ssid, password);
            delay(1000);
        }
        */
    }

    EEPROM.begin(1);
    eeprom_read();
    switch (FSM)
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
        //light blue
        setBuff(0x00, 0x40, 0x40);
      break;

      default:
      setBuff(0x00, 0x00, 0x00);
      break;

    }
    M5.dis.displaybuff(DisBuff);
    Serial.println(mntpnt[FSM]);
    
    if(ntrip_c.reqSrcTbl(host[FSM],httpPort[FSM])){
    char buffer[512];
    delay(5);
    while(ntrip_c.available()){
      ntrip_c.readLine(buffer,sizeof(buffer));
      Serial.print(buffer); 
      }
    }
    else{
      Serial.println("SourceTable request error");
    }
    Serial.print("Requesting SourceTable is OK\n");
    ntrip_c.stop(); //Need to call "stop" function for next request.
  
    if(!ntrip_c.reqRaw(host[FSM],httpPort[FSM],mntpnt[FSM],user[FSM],passwd[FSM])){
      FSM++;
      EEPROM.put(0,FSM);
      EEPROM.commit();
      delay(10000);
      ESP.restart();
  }
}

void loop() {
    delay(1000);
    if (M5.Btn.wasPressed())
      {
        FSM++;
        if (FSM >= baseCount)
          {
          FSM = 0;
          }
      EEPROM.put(0,FSM);
      EEPROM.commit();
      Serial.println(FSM);
      delay(50);
      ESP.restart();
      }
    while(ntrip_c.available()) {
      char ch = ntrip_c.read();        
      Serial2.print(ch);
      Count++;
      }
    Serial2.flush();
    
    Serial.print(host[FSM]);
    Serial.print(",");
    Serial.print(httpPort[FSM]);
    Serial.print(",");
    Serial.print(mntpnt[FSM]);
    Serial.print(",");
    Serial.print(user[FSM]);
    Serial.print(",");
 // Serial.print(passwd[FSM]);
 // Serial.print(",");
    Serial.println(Count);
    delay(1000);
    M5.update();
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
EEPROM.get(0,FSM);
if (FSM>=baseCount){
  EEPROM.put(0,0);
  EEPROM.commit();
  FSM=0;
 }
}
