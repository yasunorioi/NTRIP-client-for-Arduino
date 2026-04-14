#include"NTRIPClient.h"
bool NTRIPClient::reqSrcTbl(char* host,int &port)
{
  if(!connect(host,port)){
      Serial.print("Cannot connect to ");
      Serial.println(host);
      return false;
  }
  String p = String("GET / HTTP/1.1\r\n"
      "Host: ") + host + ":" + port + String("\r\n"
      "User-Agent: NTRIPClient for Arduino v2.0\r\n"
      "Ntrip-Version: Ntrip/2.0\r\n"
      "Connection: close\r\n"
      "\r\n");
  print(p);
  flush();
  unsigned long timeout = millis();
  while (available() == 0) {
     if (millis() - timeout > 5000) {
        Serial.println("Client Timeout !");
        stop();
        return false;
     }
     delay(10);
  }
  char buffer[50];
  readLine(buffer,sizeof(buffer));
  if(strncmp((char*)buffer,"SOURCETABLE 200 OK",17) == 0)
  {
    return true;
  }
  Serial.print("Unexpected response: ");
  Serial.print((char*)buffer);
  return false;
}
bool NTRIPClient::reqRaw(char* host,int &port,char* mntpnt,char* user,char* psw)
{
    if(!connect(host,port)){
        Serial.print("Cannot connect to ");
        Serial.println(host);
        return false;
    }
    Serial.println("Request NTRIP");

    String p = String("GET /") + mntpnt + String(" HTTP/1.1\r\n"
        "Host: ") + host + ":" + port + String("\r\n"
        "User-Agent: NTRIPClient for Arduino v2.0\r\n"
        "Ntrip-Version: Ntrip/2.0\r\n");

    if (strlen(user) > 0) {
        String auth = base64::encode(String(user) + String(":") + psw);
        #ifdef Debug
        Serial.println(String(user) + String(":") + psw);
        #endif
        p = p + String("Authorization: Basic ") + auth + String("\r\n");
    }
    p = p + String("\r\n");
    print(p);
    flush();
    #ifdef Debug
    Serial.println(p);
    #endif
    unsigned long timeout = millis();
    while (available() == 0) {
        if (millis() - timeout > 10000) {
            Serial.println("Client Timeout !");
            stop();
            return false;
        }
        delay(10);
    }
    char buffer[50];
    readLine(buffer,sizeof(buffer));
    // Accept both NTRIP v1 (ICY 200 OK) and v2 (HTTP/1.1 200 OK)
    if(strncmp((char*)buffer,"ICY 200 OK",10) == 0 ||
       strncmp((char*)buffer,"HTTP/1.1 200",12) == 0 ||
       strncmp((char*)buffer,"HTTP/1.0 200",12) == 0)
    {
      // Skip remaining HTTP headers if present (v2 sends headers)
      if(strncmp((char*)buffer,"HTTP",4) == 0) {
        unsigned long hdrTimeout = millis();
        while(available() || (millis() - hdrTimeout < 1000)) {
          if(available()) {
            char hdr[128];
            readLine(hdr, sizeof(hdr));
            if(hdr[0] == '\r' || hdr[0] == '\n') break; // empty line = end of headers
            hdrTimeout = millis(); // reset timeout on each header line
          }
          delay(1);
        }
      }
      return true;
    }
    Serial.print("Unexpected response: ");
    Serial.print((char*)buffer);
    stop();
    return false;
}
bool NTRIPClient::reqRaw(char* host,int &port,char* mntpnt)
{
    return reqRaw(host,port,mntpnt,"","");
}
int NTRIPClient::readLine(char* _buffer,int size)
{
  int len = 0;
  unsigned long timeout = millis();
  while(millis() - timeout < 1000) {
    if(available()) {
      _buffer[len] = read();
      len++;
      if(_buffer[len-1] == '\n' || len >= size - 1) break;
      timeout = millis(); // reset on each byte received
    }
    delay(1);
  }
  _buffer[len]='\0';

  return len;
}
