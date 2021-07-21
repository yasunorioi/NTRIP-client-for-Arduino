
#include "M5Atom.h"
#include <EEPROM.h>
#include <WiFi.h> 
#include "NTRIPClient.h"

const char* ssid     = "SSID";
const char* password = "pass";

uint8_t baseCount=4;
char* host[]={
  "rtk.toiso.fit",
  "rtk.toiso.fit",
  "117.102.192.33",
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
  "ENIWA-K-F9P",
  "00001122"
};
char* user[]={
  "",
  "",
  "",
  "00001122"
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
void setup() {
    M5.begin(true, false, true);
    delay(10);
    //setBuff(0xff, 0x00, 0x00);
    Serial.begin(115200);
    Serial2.begin(115200,SERIAL_8N1,22,19);
    delay(10);
    Serial.println();
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }

    EEPROM.begin(1);
    eeprom_read();
    switch (FSM)
    {
      case 0:
        setBuff(0x40, 0x00, 0x00);
      break;
      case 1:
        setBuff(0x00, 0x40, 0x00);
      break;
      case 2:
        setBuff(0x00, 0x00, 0x40);
      break;
      case 3:
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
    delay(15000);
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
