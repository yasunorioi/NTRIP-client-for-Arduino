/****************************************************************
 * 
 * This Example is used to test button
 * 
 * Arduino tools Setting 
 * -board : M5StickC
 * -Upload Speed: 115200 / 750000 / 1500000
 * 
****************************************************************/

#include "M5Atom.h"
#include <EEPROM.h>
#include <WiFi.h> 
#include "NTRIPClient.h"

const char* ssid     = "ssid";
const char* password = "pass";
uint8_t baseCount = 4;
uint8_t FSM;

void eeprom_read(){
EEPROM.get(0,FSM);
if (FSM>=baseCount){
  EEPROM.put(0,0);
  EEPROM.commit();
  FSM=0;
 }
/* Serial.print("EEPROM=");
 Serial.println(FSM);
*/
}

uint8_t DisBuff[2 + 5 * 5 * 3];

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

char* host;
int httpPort;
char* mntpnt;
char* user;
char* passwd;
NTRIPClient ntrip_c;

void setup()
{
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
            host = "rtk.toiso.fit";
            httpPort = 2101;
            mntpnt = "eniwa-bd982cmr";
            user = "";
            passwd = "";
            break;
        case 1:
            setBuff(0x00, 0x40, 0x00);
            host = "asus.toiso.fit";
            httpPort = 2101;
            mntpnt = "eniwa-bd982";
            user = "";
            passwd = "";
            break;
        case 2:
            setBuff(0x00, 0x00, 0x40);
            host = "117.102.192.33";
            httpPort = 2101;
            mntpnt = "ENIWA-K-F9P";
            user = "";
            passwd = "";
            break;
        case 3:
            setBuff(0x20, 0x20, 0x20);
            host = "rtk.toiso.fit";
            httpPort = 2101;
            mntpnt = "eniwa-f9p";
            user = "";
            passwd = "";
            break;
        default:
            break;
        }
    M5.dis.displaybuff(DisBuff);
    Serial.println(mntpnt);
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
  
    if(!ntrip_c.reqRaw(host,httpPort,mntpnt,user,passwd)){
    delay(15000);
    ESP.restart();
  }
}

//uint8_t FSM = 1;

void loop()
{
  delay(1000);
    if (M5.Btn.wasPressed())
    {
        FSM++;
        if (FSM >= baseCount)
        {
            FSM = 0;
            
        }
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
            setBuff(0x20, 0x20, 0x20);
            break;
        default:
            break;
        }
        M5.dis.displaybuff(DisBuff);
        EEPROM.put(0,FSM);
        EEPROM.commit();
        Serial.println();
        Serial.println(FSM);
        

    delay(50);
    ESP.restart();
    }
  while(ntrip_c.available()) {
        char ch = ntrip_c.read();        
        Serial2.print(ch);
        
  }
  Serial2.flush();
  delay(50);
  M5.update();
    
}
