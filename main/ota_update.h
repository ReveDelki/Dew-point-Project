#pragma once
#include <ESPAsyncWebServer.h>

/*
  OTA routes (friend style):
  - /updateAction?type=firmware   => U_FLASH
  - /updateAction?type=filesystem => U_SPIFFS
*/

void setupOTA(AsyncWebServer& server);
