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
#include <EEPROM.h>
#include "NTRIPClient.h"
#include "eniwa-agriICT.h"

NTRIPClient ntrip_c;

uint8_t DisBuff[2 + 5 * 5 * 3];
uint8_t FSM;
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
  Serial.println("Requesting SourceTable.");
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
  
  Serial.println("Requesting MountPoint's Raw data");
  if(!ntrip_c.reqRaw(host[FSM],httpPort[FSM],mntpnt[FSM],user[FSM],passwd[FSM])){
    FSM++;
    EEPROM.put(0,FSM);
    EEPROM.commit();
    delay(15000);
    ESP.restart();
  }
  Serial.println("Requesting MountPoint is OK");
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
  // put your main code here, to run repeatedly:
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


void eeprom_read(){
EEPROM.get(0,FSM);
if (FSM>=baseCount){
  EEPROM.put(0,0);
  EEPROM.commit();
  FSM=0;
 }
}

