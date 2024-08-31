/*
 *  NTRIP client for Arduino Ver. 1.0.0 
 *  NTRIPClient Sample
 *  Request Source Table (Source Table is basestation list in NTRIP Caster)
 *  Request Reference Data 
 * 
 * 
 */
//#include <ESP8266WiFi.h>  //Need for ESP8266
//#include <WiFi.h>           //Need for ESP32 
#include "NTRIPClient.h"
#include <M5Stack.h>
#include <SPI.h>

#include <Ethernet2.h>
EthernetClient client;
/* #include <ESPmDNS.h>
const char* MDNS_NAME="m5stack";
*/

#define SCK  18
#define MISO 19
#define MOSI 23
#define CS   5

char uart_buffer[8]    = {0x01, 0x05, 0x00, 0x01, 0x02, 0x00, 0x9d, 0x6a};
char uart_rx_buffer[8] = {0};

char Num                   = 0;
char stringnum             = 0;
unsigned long W5500DataNum = 0;
unsigned long Send_Num_Ok  = 0;
unsigned long Rec_Num      = 0;
unsigned long Rec_Num_Ok   = 0;

/*
const char* ssid     = "your_ssid";
const char* password = "your_password";
*/

//char* host = "192.168.10.9";
 char* host = "rtk.toiso.fit";
int httpPort = 2101; //port 2101 is default port of NTRIP caster
//char* mntpnt = "eniwa-ubx2";
char* mntpnt = "eniwa-bd982";

char* user   = ""; //ntrip caster's client user
char* passwd = ""; //ntrip caster's client password
NTRIPClient ntrip_c;

uint64_t Count;

byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
//IPAddress ip(192, 168, 10, 177); //Static IP address

void setup() {
    // put your setup code here, to run once:
    M5.begin(true, false, true);
    delay(10);
    SPI.begin(SCK, MISO, MOSI, -1);
    Ethernet.init(CS);
    // start the Ethernet connection and the server:
    
  //  Ethernet.begin(mac, ip); //Static IP
    Ethernet.begin(mac); // DHCP
    Serial.println(Ethernet.localIP());

    M5.Lcd.println("M5Stack W5500 NTRIP");
    M5.Lcd.println(" ");
    M5.Lcd.println(Ethernet.localIP());
    //Serial2.begin(115200, SERIAL_8N1,5,15);
    Serial2.begin(115200, SERIAL_8N1,16,17);
    //Serial2.begin(115200,SERIAL_8N1,13,14);
    Serial.println(Ethernet.localIP());
    Serial.println("Requesting SourceTable.");
    /*const char* server = host;
    IPAddress server_ip = MDNS.queryHost(server);
    Serial.print("Ntrip server IP");
    Serial.println(server_ip);
    */
    if(ntrip_c.reqSrcTbl(host,httpPort)){
      char buffer[512];
      delay(5);
        while(ntrip_c.available()){
          ntrip_c.readLine(buffer,sizeof(buffer));
          Serial.print(buffer);
          M5.Lcd.println(buffer);
        }
    }
    else{
      Serial.println("SourceTable request error");
    }
    Serial.print("Requesting SourceTable is OK\n");
    ntrip_c.stop(); //Need to call "stop" function for next request.
    Serial.println("Requesting MountPoint's Raw data");
    M5.Lcd.print("ntrip://");
    M5.Lcd.print(user);
    M5.Lcd.print(":");
    M5.Lcd.print(passwd);
    M5.Lcd.print("@");
    M5.Lcd.print(host);
    M5.Lcd.print(":");
    M5.Lcd.print(httpPort);
    M5.Lcd.print("/");
    M5.Lcd.println(mntpnt);
    if(!ntrip_c.reqRaw(host,httpPort,mntpnt,user,passwd)){
      delay(15000);
      ESP.restart();
      }
    Serial.println("Requesting MountPoint is OK");
    M5.Lcd.println("Requesting MountPoint is OK");
    M5.Lcd.setTextSize(3);

    //mDNS
    /*
    if(!MDNS.begin(MDNS_NAME)){
    Serial.print("Error MDNS_NAME:");
    Serial.println(MDNS_NAME);
    delay(10000);
    ESP.restart();

  }
  */

}

void loop() {
  // put your main code here, to run repeatedly:
  while(ntrip_c.available()) {
        char ch = ntrip_c.read();        
        Serial2.print(ch);
        Serial2.flush();
        Count++;
  }
  M5.Lcd.setCursor(0,150);
  M5.Lcd.println(Count);
}
