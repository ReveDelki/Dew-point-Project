/*
  OTA (Update.h upload endpoint like friend)
  - /updateAction?type=firmware   => U_FLASH
  - /updateAction?type=filesystem => U_SPIFFS

  NOTE: Firmware OTA requires OTA partitions (otadata + ota_0 + ota_1)
*/

#include <Arduino.h>
#include <Update.h>     
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>

#include "ota_update.h"

void setupOTA(AsyncWebServer& server){

  // --------- Single OTA endpoint ---------
  server.on(
    "/updateAction",
    HTTP_POST,

    // ===== Finished callback (send response) =====
    [](AsyncWebServerRequest *request){
      bool ok = !Update.hasError();

      // Reply then reboot after disconnect 
      request->send(
        200,
        "text/plain",
        ok ? "Update Success. Rebooting..." : "Update Failed"
      );

      if (ok) {
        request->onDisconnect([](){
          delay(200);
          ESP.restart();
        });
      }
    },

    // ===== Upload handler =====
    [](AsyncWebServerRequest *request,
       const String& filename,
       size_t index,
       uint8_t *data,
       size_t len,
       bool final){

      // U_FLASH or U_SPIFFS memory?
      static int updateType = -1;

      // ---------- Upload start ----------
      if (index == 0) {

        updateType = -1;

        // Determine OTA type from query
        if (!request->hasParam("type")) {
          Serial.println("[OTA] Missing type argument");
          request->send(400, "text/plain",
                        "Missing type (?type=firmware|filesystem)");
          return;
        }

        String type = request->getParam("type")->value();
        Serial.print("[OTA] type = ");
        Serial.println(type);

        if (type == "firmware") {
          updateType = U_FLASH;       // Firmware OTA
          Serial.print("[OTA] Firmware OTA start: ");
        }
        else if (type == "filesystem") {
          updateType = U_SPIFFS;      // LittleFS
          Serial.print("[OTA] Filesystem OTA start: ");
          LittleFS.end();             // End LittleFS to free SPIFFS/LittleFS OTA partition for writing
        }
        else {
          Serial.print("[OTA] --> Unknown update file type !!!\n");
          request->send(400, "text/plain",
                        "Unknown type (use firmware or filesystem)");
          return;
        }

        Serial.printf("%s\n", filename.c_str());

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateType)) {
          Update.printError(Serial);
        }
      }

      // ---------- Write chunk ----------
      if (len) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }

      // ---------- Finalize ----------
      if (final) {
        if (Update.end(true)) {
          Serial.printf("[OTA] Update Success: %u bytes\n",
                        (unsigned)(index + len));
        } else {
          Update.printError(Serial);
        }
      }
    }
  );
}

