/*
 *  NTRIP client for Arduino Ver. 1.0.0 
 *  NTRIPClient Sample
 *  Request Source Table (Source Table is basestation list in NTRIP Caster)
 *  Request Reference Data 
 * 
 * 
 */
#include "M5Atom.h"
#include "esp_wps.h"
#include <WiFi.h>           //Need for ESP32 
#include "wpsConnector.h"
#include "NTRIPClient.h"

NTRIPClient ntrip_c;

char* host     = "rtk.toiso.fit";
int   httpPort = 2101;
char* mntpnt   = "eniwa-bd982";
char* user     = "";
char* passwd   = "";

uint8_t DisBuff[2 + 5 * 5 * 3];
uint64_t Count;
uint8_t WiFiCount;
uint8_t WiFiStatus;

unsigned long prev,next,interval;

void setup() {
  // put your setup code here, to run once:
  pinMode(0,OUTPUT);
  digitalWrite(0,LOW);

  // start timer
  prev=0;
  interval=1000;
  
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
    Serial.println("Starting WPS");
    WiFi.disconnect();
    wpsConnect();
  } 
  }
  Serial.println(WiFi.SSID());
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  setBuff(0x40, 0x00, 0x00);
  M5.dis.displaybuff(DisBuff);
  Serial.println(mntpnt);
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
  delay(1000);
  while(ntrip_c.available()) {
    char ch = ntrip_c.read();
    Serial2.print(ch);
    Count++;
  }
  Serial2.flush();

  unsigned long curr=millis();
  if ((curr - prev) >= interval){
    Serial.print("bit:");
    Serial.println(Count);
    prev=curr;
  }
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



