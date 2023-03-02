/*
 *  NTRIP client for Arduino Ver. 1.0.0 
 *  NTRIPClient Sample
 *  Request Source Table (Source Table is basestation list in NTRIP Caster)
 *  Request Reference Data 
 * 
 * 
 */
//#include <ESP8266WiFi.h>  //Need for ESP8266
#include "M5Atom.h"
#include "esp_wps.h"
#include <WiFi.h>           //Need for ESP32 
#include "wpsConnector.h"
#include "NTRIPClient.h"


char* host = "rtk.toiso.fit";
int httpPort = 2101; //port 2101 is default port of NTRIP caster
char* mntpnt = "eniwa-bd982";
char* user   = "";
char* passwd = "";
NTRIPClient ntrip_c;


void setup() {
  // put your setup code here, to run once:
  pinMode(0,OUTPUT);
  digitalWrite(0,LOW);
  Serial.begin(115200);
  Serial2.begin(115200,SERIAL_8N1,22,19);

  M5.begin(true, false, true);
  delay(10);
  WiFi.begin();
  int wificount=0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wificount++;
  
  if (wificount == 5){
    WiFi.disconnect();
    Serial.println("Starting WPS");
    WiFi.disconnect();
    wpsConnect();
  } 
  }
  Serial.println(WiFi.SSID());
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  
  Serial.println("Requesting SourceTable.");
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
  }
  Serial.print("Requesting SourceTable is OK\n");
  
  ntrip_c.stop(); //Need to call "stop" function for next request.
  
  Serial.println("Requesting MountPoint's Raw data");
  if(!ntrip_c.reqRaw(host,httpPort,mntpnt,user,passwd)){
    delay(15000);
    ESP.restart();
  }
  Serial.println("Requesting MountPoint is OK");

  
}

void loop() {
  // put your main code here, to run repeatedly:
  while(ntrip_c.available()) {
        char ch = ntrip_c.read();        
        Serial2.print(ch);
        
  }
}
