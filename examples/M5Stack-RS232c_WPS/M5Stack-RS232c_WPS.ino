/*
 *  NTRIP client for Arduino Ver. 1.0.0 
 *  NTRIPClient Sample
 *  Request Source Table (Source Table is basestation list in NTRIP Caster)
 *  Request Reference Data 
 * 
 * 
 */
//#include <ESP8266WiFi.h>  //Need for ESP8266
#include "NTRIPClient.h"
#include <EEPROM.h>
#include <M5Stack.h>
#include "wpsConnector.h"
//#include <WiFiManager.h> 
#include "eniwa-agriICT.h"
int uart_bps=115200;

hw_timer_t* timer = NULL;

unsigned long countnum;
int num;

void IRAM_ATTR onTimer(){
  num = countnum;
  countnum=0;
}
// https://github.com/tzapu/WiFiManager
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
NTRIPClient ntrip_c;
uint8_t DisBuff[2 + 5 * 5 * 3];
uint8_t FSM;
uint8_t WiFiCount;
uint8_t WiFiStatus;
int oldnum;

void setup() {
   // put your setup code here, to run once:
    M5.begin(true, false, true);
    delay(10);
    M5.Lcd.setTextSize(2);  
    M5.Lcd.setCursor(0,0);
    M5.Lcd.println("WiFi setup");
    M5.Lcd.println("WPS Connecting...");
    Serial2.begin(uart_bps, SERIAL_8N1,16,17);  
    WiFi.begin();
    int wificount=0;
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      wificount++;
      if (wificount == 5){
        Serial.println("Starting WPS");
        WiFi.disconnect();
        wpsConnect();
      }
    }
    Serial.println(WiFi.SSID());
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
    configTime(9 * 3600, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");
    //if you get here you have connected to the WiFi    
    Serial.println("connected...yeey :)");
    M5.Lcd.println("connected...yeey :)");
    EEPROM.begin(1);
    EEPROM.get(0,FSM);
    if (FSM>=baseCount){
      EEPROM.put(0,0);
      EEPROM.commit();
      FSM=0;
    }
    Serial.println(mntpnt[FSM]);
    Serial.println("Requesting SourceTable.");
    M5.Lcd.println("Requesting SourceTable.");
    if(ntrip_c.reqSrcTbl(host[FSM],httpPort[FSM])){
      char buffer[512];
      delay(5);
      while(ntrip_c.available()){
      ntrip_c.readLine(buffer,sizeof(buffer));
      Serial.print(buffer); 
    }
  } else {
    Serial.println("SourceTable request error");
    M5.Lcd.print("SourceTable request error");
  }
    Serial.print("Requesting SourceTable is OK\n");
    M5.Lcd.print("Requesting SourceTable is OK\n");
    ntrip_c.stop(); //Need to call "stop" function for next request.

    Serial.println("Requesting MountPoint's Raw data");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(1);  
    M5.Lcd.setCursor(0,0);
    M5.Lcd.print("ntrip://");
    M5.Lcd.print(user[FSM]);
    M5.Lcd.print(":");
    M5.Lcd.print(passwd[FSM]);
    M5.Lcd.print("@");
    M5.Lcd.print(host[FSM]);
    M5.Lcd.print(":");
    M5.Lcd.print(httpPort[FSM]);
    M5.Lcd.print("/");
    M5.Lcd.println(mntpnt[FSM]);
    if(!ntrip_c.reqRaw(host[FSM],httpPort[FSM],mntpnt[FSM],user[FSM],passwd[FSM])){
      FSM++;
      EEPROM.put(0,FSM);
      EEPROM.commit();
      delay(10);
      ESP.restart();
    }
    timer =timerBegin(0,getApbFrequency()/1000000,true);
    timerAttachInterrupt(timer,&onTimer,true);
    timerAlarmWrite(timer,1000000*1,true);
    timerAlarmEnable(timer);
    timerRestart(timer);
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed())
   {
     FSM++;
     if (FSM >= baseCount){ FSM = 0;}
      EEPROM.put(0,FSM);
      EEPROM.commit();
      Serial.println(FSM);
      M5.Lcd.setCursor(0,48);
      M5.Lcd.println("Change NTRIP Server");
      delay(50);
      ESP.restart();
    }
    M5.Lcd.setTextSize(4);  
    M5.Lcd.setCursor(0,48);
    while(ntrip_c.available()) {
     char ch = ntrip_c.read();
     countnum++;
     Serial2.print(ch);
    }
     Serial2.flush();
  /*
   *  512*8 buffer
   * 
  while(ntrip_c.available()) {
    rtcmData[rtcmCount++] = ntrip_c.read();
    if (rtcmCount == sizeof(rtcmData)){
      rtcmCount = 0;
      break;
    }
  }

 if (rtcmCount>2048){
    for(size_t i=0 ; i <= rtcmCount ; i++){
      Serial2.print(rtcmData[i]);
    }
    count++;
  }*/

 // Serial.println(countnum);
 if (oldnum!=num){
    //M5.Lcd.print(countnum);
    M5.Lcd.print(num*8);
    M5.Lcd.setTextSize(2);
    M5.Lcd.print(" B/s");
    M5.Lcd.setCursor(0,224);
    M5.Lcd.print("RS232c:");
    M5.Lcd.print(uart_bps);
    M5.Lcd.println("bps");
    num==oldnum;
 }
}
