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
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
// WiFiAP:"M5Atom" Password:"m5atompass"
/*
Maybe you need WiFiAP fresh setup.
https://github.com/espressif/arduino-esp32/issues/400#issuecomment-411076993

#include <WiFi.h>
void setup() {
  // put your setup code here, to run once:
WiFi.disconnect(true);   // still not erasing the ssid/pw. Will happily reconnect on next start
WiFi.begin("0","0");       // adding this effectively seems to erase the previous stored SSID/PW
ESP.restart();
delay(1000);
}

void loop() {
  // put your main code here, to run repeatedly:

} 

*/
 
/*const char* ssid     = "SSID";
const char* password = "pass";
*/

uint8_t baseCount=4;
char* host[]={
  //Red
  "rtk.toiso.fit",
  //Green
//  "rtk.toiso.fit",
  //Blue
//  "117.102.192.33",
  //light blue
  "rtk.toiso.fit"
};
int httpPort[]={
  2101,
//  2101,
//  2101,
  2101
};
char* mntpnt[]={
  "eniwa-bd982",
//  "eniwa-f9p",
//  "eniwa-kazui",
  "eniwa-bd970"
};
char* user[]={
  "",
//  "",
//  "",
  ""
};
char* passwd[]={
  "",
//  "",
//  "",
  ""
};

NTRIPClient ntrip_c;
uint8_t DisBuff[2 + 5 * 5 * 3];
uint8_t FSM;
uint64_t Count;
uint8_t WiFiCount;
uint8_t WiFiStatus;

void setup() {
    pinMode(0,OUTPUT);
    digitalWrite(0,LOW);
    Serial.begin(115200);
    Serial2.begin(115200,SERIAL_8N1,22,19);
    M5.begin(true, false, true);
    delay(10);
    //setBuff(0xff, 0x00, 0x00);

    delay(10);
    Serial.println();
    WiFi.mode(WIFI_STA); 
    WiFiManager wm;
    bool res;
    res = wm.autoConnect("M5Atom","m5atompass"); // password protected ap

    if(!res) {
        Serial.println("Failed to connect");
        // ESP.restart();
    }
    else {
        //if you get here you have connected to the WiFi    
        Serial.println("connected...yeey :)");
    
/*    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
/*      WiFiCount++;
      Serial.print(".");
            if (WiFiCount == 5 || WiFiStatus == WL_NO_SSID_AVAIL) {
            esp_wifi_restore();
            WiFi.begin(ssid, password);
            delay(1000);
        }
        
    }*/

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
        //pink
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
