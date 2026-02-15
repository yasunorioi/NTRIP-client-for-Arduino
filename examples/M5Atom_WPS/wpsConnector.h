/*
 * https://qiita.com/coppercele/items/6789deea453826916725
 */
#pragma once
#include <Arduino.h>
#include "esp_wps.h"
#include "WiFi.h"

#define ESP_WPS_MODE WPS_TYPE_PBC
#define ESP_MANUFACTURER "ESPRESSIF"
#define ESP_MODEL_NUMBER "ESP32"
#define ESP_MODEL_NAME "ESPRESSIF IOT"
#define ESP_DEVICE_NAME "ESP STATION"

void wpsInitConfig();
void wpsStart();
void wpsStop();
String wpspin2string(uint8_t a[]);
void WiFiEvent(WiFiEvent_t event, arduino_event_info_t info);
void wpsConnect();