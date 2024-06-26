/*
 * https://qiita.com/coppercele/items/6789deea453826916725
 */
#include "wpsConnector.h"

static esp_wps_config_t config;

void wpsInitConfig() {
  config.wps_type = ESP_WPS_MODE;
  strcpy(config.factory_info.manufacturer, ESP_MANUFACTURER);
  strcpy(config.factory_info.model_number, ESP_MODEL_NUMBER);
  strcpy(config.factory_info.model_name, ESP_MODEL_NAME);
  strcpy(config.factory_info.device_name, ESP_DEVICE_NAME);
}

void wpsStart() {
  if (esp_wifi_wps_enable(&config)) {
    Serial.println("WPS Enable Failed");
  }
  else if (esp_wifi_wps_start(0)) {
    Serial.println("WPS Start Failed");
  }
}

void wpsStop() {
  if (esp_wifi_wps_disable()) {
    Serial.println("WPS Disable Failed");
  }
}

String wpspin2string(uint8_t a[]) {
  char wps_pin[9];
  for (int i = 0; i < 8; i++) {
    wps_pin[i] = a[i];
  }
  wps_pin[8] = '\0';
  return (String)wps_pin;
}

void WiFiEvent(WiFiEvent_t event, arduino_event_info_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println("Station Mode Started");
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.println("Connected to :" + String(WiFi.SSID()));
    Serial.print("Got IP: ");
    Serial.println(WiFi.localIP());
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.println("Disconnected from station, attempting reconnection");
    WiFi.reconnect();
    break;
  case ARDUINO_EVENT_WPS_ER_SUCCESS:
    Serial.println("WPS Successfull, stopping WPS and connecting to: " +
                   String(WiFi.SSID()));
    wpsStop();
    delay(10);
    WiFi.begin();
    break;
  case ARDUINO_EVENT_WPS_ER_FAILED:
    Serial.println("WPS Failed, retrying");
    wpsStop();
    wpsStart();
    break;
  case ARDUINO_EVENT_WPS_ER_TIMEOUT:
    Serial.println("WPS Timedout, retrying");
    wpsStop();
    wpsStart();
    break;
  case ARDUINO_EVENT_WPS_ER_PIN:
    Serial.println("WPS_PIN = " + wpspin2string(info.wps_er_pin.pin_code));
    break;
  default:
    break;
  }
}

void wpsConnect() {
  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_MODE_STA);
  Serial.println("Starting WPS");
  wpsInitConfig();
  wpsStart();
}