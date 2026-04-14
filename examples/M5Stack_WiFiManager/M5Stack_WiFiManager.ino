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
#include <M5Stack.h>
#include <WiFiManager.h>

// https://github.com/tzapu/WiFiManager
// WiFiAP:"M5Atom" Password:"m5stackpass"
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

char* host     = "rtk.toiso.fit";
int   httpPort = 2101;
char* mntpnt   = "eniwa-bd982";
char* user     = "";
char* passwd   = "";

uint64_t Count;
int uart_bps=115200;

void setup() {
   // put your setup code here, to run once:
    M5.begin(true, false, true);
    delay(10);
    M5.Lcd.setTextSize(2);  
    M5.Lcd.setCursor(0,0);
    M5.Lcd.println("WiFi setup");
    M5.Lcd.println("SSID:M5stack");
    M5.Lcd.println("pass:m5stackpass");
    M5.Lcd.println("http://192.168.4.1/");
    M5.Lcd.println("");
    M5.Lcd.println("WiFi Connecting...");

    Serial2.begin(uart_bps, SERIAL_8N1,16,17);
    Serial.println("Requesting SourceTable.");
    WiFi.mode(WIFI_STA); 
    WiFiManager wm;
    bool res;
    res = wm.autoConnect("M5stack","m5stackpass"); // password protected ap

    if(!res) {
      Serial.println("Failed to connect");
       // ESP.restart();
    }
    else {
      //if you get here you have connected to the WiFi    
       Serial.println("connected...yeey :)");
       M5.Lcd.println("connected...yeey :)");

       Serial.println(mntpnt);
       Serial.println("Requesting SourceTable.");
       M5.Lcd.println("Requesting SourceTable.");
       if(ntrip_c.reqSrcTbl(host,httpPort)){
         char buffer[512];
         delay(5);
         while(ntrip_c.available()){
           ntrip_c.readLine(buffer,sizeof(buffer));
           Serial.print(buffer);
         }
       }
       else{
         Serial.println("SourceTable request error");
         M5.Lcd.print("SourceTable request error");
       }
       Serial.print("Requesting SourceTable is OK\n");
       M5.Lcd.print("Requesting SourceTable is OK\n");
       ntrip_c.stop();

       Serial.println("Requesting MountPoint's Raw data");
       M5.Lcd.fillScreen(BLACK);
       M5.Lcd.setTextSize(1);
       M5.Lcd.setCursor(0,0);
       M5.Lcd.print("ntrip://");
       M5.Lcd.print(host);
       M5.Lcd.print(":");
       M5.Lcd.print(httpPort);
       M5.Lcd.print("/");
       M5.Lcd.println(mntpnt);
       if(!ntrip_c.reqRaw(host,httpPort,mntpnt,user,passwd)){
         delay(10000);
         ESP.restart();
       }
       Serial.println("Requesting MountPoint is OK");
       M5.Lcd.println("Requesting MountPoint is OK");
  }
}

void loop() {
  while(ntrip_c.available()) {
    char ch = ntrip_c.read();
    Serial2.print(ch);
    Count++;
  }
  Serial2.flush();
  Serial.print(host);
  Serial.print(":");
  Serial.print(httpPort);
  Serial.print("/");
  Serial.print(mntpnt);
  Serial.print(" ");
  Serial.println(Count);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(0,48);
  M5.Lcd.println(Count);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(0,224);
  M5.Lcd.print("RS232c:");
  M5.Lcd.print(uart_bps);
  M5.Lcd.println("bps");
  M5.update();
}
