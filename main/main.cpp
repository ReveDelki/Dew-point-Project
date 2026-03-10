/*
  ESP32 + LilyGO T-Display + two SHT4x sensors
  Automatic fan control based on dew point difference or manual turn off via web UI
  AsyncWebServer, UI assets served from LittleFS (/data)
*/

#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_Sensor.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <SD.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "ota_update.h"  // OTA (see ota_update.h/ota_update.cpp for details)
#include "sd_logger.h"   // SD logging (see sd_logger.h/sd_logger.cpp for details)

/************************************************************************************/

// ------- Pins / constants -------
#define I2C_SDA    21
#define I2C_SCL    22
#define I2C_SDA1   32
#define I2C_SCL1   13
#define SHT4X_ADDR 0x44
#define FAN_PIN    15

static const int SCR_W = 240, SCR_H = 135;
const int UI_X_SHIFT = 12;   // +12 px to the right (change value you like)

// ------- WiFi -------
// Input your Wi-Fi name and pass in here (="name" and "password" of your Wi-Fi network).
const char* WIFI_SSID = "STH4x_WiFi";
const char* WIFI_PASS = "12345678";

AsyncWebServer server(80);

// ------- Sensors / state -------
Adafruit_SHT4x sht4_A, sht4_B;
TwoWire Wire1I2C = TwoWire(1);
bool hasA=false, hasB=false;

float tempA=NAN, rhA=NAN, dewA=NAN;
float tempB=NAN, rhB=NAN, dewB=NAN;

bool fanOn=false;         // hysteresis state
bool fanDisabled=false;   // manual off

const float DELTA_ON_HYST  = 5.0f;
const float DELTA_OFF_EDGE = 2.0f;

// ------- TFT -------
TFT_eSPI tft;
TFT_eSprite screen = TFT_eSprite(&tft);

// ------- Reboot flag (kept but not required with onDisconnect) -------
volatile uint32_t REBOOT_AT_MS = 0;  // 0 = no reboot scheduled

// ------- SD logging gate -------
// OFF by default; will turn ON only after web browser opens page and calls /startLogging
volatile bool sdLoggingEnabled = false;

// ------- Helpers -------
// i2cPresent: check if device with addr responds on bus
static inline bool i2cPresent(TwoWire& bus, uint8_t addr){
  bus.beginTransmission(addr);
  return (bus.endTransmission()==0);
}

// dewPointC: Magnus formula
static inline float dewPointC(float T, float RH){
  const float a=17.62f, b=243.12f;
  float g = logf(RH*0.01f) + (a*T)/(b+T);
  return (b*g)/(a-g);
}

// fmt1: format float with 1 decimal or "--" if NaN/Inf
static inline String fmt1(float v){ return isfinite(v) ? String(v,1) : String("--"); }

// drawCentered, drawLeft: helper to draw text on TFT
static inline void drawCentered(const String& s,int x,int y,int font,uint16_t color){
  screen.setTextColor(color, TFT_BLACK);
  screen.setTextDatum(MC_DATUM);
  screen.drawString(s, x, y, font);
}
static inline void drawLeft(const String& s,int x,int y,int font,uint16_t color){
  screen.setTextColor(color, TFT_BLACK);
  screen.setTextDatum(TL_DATUM);
  screen.drawString(s, x, y, font);
}

// ------- Time helpers -------
// timeValid: true if system time is set (after /startLogging or NTP etc.)
static inline bool timeValid(){ 
  return time(nullptr) > 1700000000; 
}

// setTimeFromEpoch: set ESP32 system time from epoch seconds
static inline void setTimeFromEpoch(time_t epoch){
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  // Germany CET/CEST (optional). Change if you want.
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();
}

// ================= RTOS shared state =================
// Shared reading state for display + /data endpoint
struct SharedState {
  bool hasA=false, hasB=false;
  float tempA=NAN, rhA=NAN, dewA=NAN;
  float tempB=NAN, rhB=NAN, dewB=NAN;
  float dewDelta=NAN;
  bool fanOn=false;
  bool fanDisabled=false;
  bool effectiveFan=false;
};

static SharedState g_state;
static SemaphoreHandle_t g_stateMutex = nullptr;

// Queue from Sensor task -> SD task (one sample every 5 minutes)
static QueueHandle_t g_sdQueue = nullptr;

// Measurement period: 5 minutes
//static const uint32_t MEAS_PERIOD_MS = 5UL * 60UL * 1000UL;
static const uint32_t MEAS_PERIOD_MS = 5UL * 1000UL;
/************************************************************************************/

// ================= RTOS TASK: Sensor + fan control =================
void TaskSensor(void*){
  // Run forever
  for(;;){

    // Read sensors 
    sensors_event_t hA, tA, hB, tB;

    bool localHasA=false, localHasB=false;
    float localTempA=NAN, localRhA=NAN, localDewA=NAN;
    float localTempB=NAN, localRhB=NAN, localDewB=NAN;

    // Sensor A
    if (!i2cPresent(Wire, SHT4X_ADDR)) {
      localHasA=false;
    } else {
      localHasA = true; // device exists
      if (!sht4_A.begin(&Wire)) {
        localHasA=false;
      } else {
        sht4_A.setPrecision(SHT4X_HIGH_PRECISION);
        sht4_A.setHeater(SHT4X_NO_HEATER);
        if (sht4_A.getEvent(&hA,&tA)) {
          localTempA=tA.temperature;
          localRhA=hA.relative_humidity;
          localDewA=dewPointC(localTempA, localRhA);
        }
      }
    }

    // Sensor B
    if (!i2cPresent(Wire1I2C, SHT4X_ADDR)) {
      localHasB=false;
    } else {
      localHasB = true;
      if (!sht4_B.begin(&Wire1I2C)) {
        localHasB=false;
      } else {
        sht4_B.setPrecision(SHT4X_HIGH_PRECISION);
        sht4_B.setHeater(SHT4X_NO_HEATER);
        if (sht4_B.getEvent(&hB,&tB)) {
          localTempB=tB.temperature;
          localRhB=hB.relative_humidity;
          localDewB=dewPointC(localTempB, localRhB);
        }
      }
    }

    // Fan logic 
    float localDelta=NAN;
    bool localFanOn=false;

    // Read previous fanOn for hysteresis memory
    bool prevFanOn=false;
    bool localFanDisabled=false;

    if (g_stateMutex) {
      xSemaphoreTake(g_stateMutex, portMAX_DELAY);
      prevFanOn = g_state.fanOn;
      localFanDisabled = g_state.fanDisabled;
      xSemaphoreGive(g_stateMutex);
    }

    localFanOn = prevFanOn;

    if (isfinite(localDewA) && isfinite(localDewB)) {
      localDelta = localDewA - localDewB;
      if (localDelta > DELTA_ON_HYST) localFanOn = true;
      if (localDelta <= DELTA_OFF_EDGE) localFanOn = false;
    } else {
      localFanOn = false;
    }

    bool localEffective = localFanOn && !localFanDisabled;
    digitalWrite(FAN_PIN, localEffective ? HIGH : LOW);

    // Update shared state for UI and /data
    if (g_stateMutex) {
      xSemaphoreTake(g_stateMutex, portMAX_DELAY);

      g_state.hasA = localHasA;
      g_state.hasB = localHasB;

      g_state.tempA = localTempA; g_state.rhA = localRhA; g_state.dewA = localDewA;
      g_state.tempB = localTempB; g_state.rhB = localRhB; g_state.dewB = localDewB;
      g_state.dewDelta = localDelta;

      g_state.fanOn = localFanOn;
      // fanDisabled stays from web UI
      g_state.effectiveFan = localEffective;

      xSemaphoreGive(g_stateMutex);
    }

    // Send one sample to SD task (if queue exists)
    if (g_sdQueue) {
      SDLogSample s;
      s.tempA = localTempA; s.rhA = localRhA; s.dewA = localDewA;
      s.tempB = localTempB; s.rhB = localRhB; s.dewB = localDewB;
      s.deltaDew = localDelta;
      s.fan = localEffective;
      s.disabled = localFanDisabled;

      // Queue length is small, but we only push every 5 minutes
      xQueueSend(g_sdQueue, &s, 0);
    }

    // Also check purge (hourly throttle inside sd_logger.cpp)
    sdLoggerPurgeIfDue();

    // Sleep until next measurement (5 minutes)
    vTaskDelay(pdMS_TO_TICKS(MEAS_PERIOD_MS));
  }
}

// ================= RTOS TASK: SD writer =================
void TaskSD(void*){
  for(;;){
    SDLogSample s;
    // Wait for a sample from sensor task
    if (xQueueReceive(g_sdQueue, &s, portMAX_DELAY) == pdTRUE) {

      // Only write when web page enabled logging and time is valid
      if (sdLoggingEnabled && timeValid()) {
        sdLoggerWriteRow(s);
      }
    }
  }
}

// ================= RTOS TASK: TFT display =================
void TaskDisplay(void*){
  for(;;){
    SharedState local;

    if (g_stateMutex) {
      xSemaphoreTake(g_stateMutex, portMAX_DELAY);
      local = g_state;
      xSemaphoreGive(g_stateMutex);
    }

    // TFT UI (your same drawing style)
    screen.fillSprite(TFT_BLACK);
    drawCentered("Moisture & Temp", SCR_W/2, 16, 4, TFT_CYAN);

    screen.drawLine(SCR_W/2 + UI_X_SHIFT, 34, SCR_W/2 + UI_X_SHIFT, SCR_H-6, TFT_DARKGREY);

    // Left block (Sensor A)
    drawCentered("Sensor A", SCR_W/4 + UI_X_SHIFT, 42, 2, TFT_WHITE);
    drawLeft("Temp: " + fmt1(local.tempA) + " C", 12 + UI_X_SHIFT, 56, 2, TFT_GREEN);
    drawLeft("RH  : " + fmt1(local.rhA)   + " %", 12 + UI_X_SHIFT, 76, 2, TFT_GREEN);
    drawLeft("Dew : " + fmt1(local.dewA)  + " C", 12 + UI_X_SHIFT, 96, 2, TFT_GREEN);

    // Right block (Sensor B)
    drawCentered("Sensor B", 3*SCR_W/4 + UI_X_SHIFT, 42, 2, TFT_WHITE);
    drawLeft("Temp: " + fmt1(local.tempB) + " C", SCR_W/2 + 12 + UI_X_SHIFT, 56, 2, TFT_YELLOW);
    drawLeft("RH  : " + fmt1(local.rhB)   + " %", SCR_W/2 + 12 + UI_X_SHIFT, 76, 2, TFT_YELLOW);
    drawLeft("Dew : " + fmt1(local.dewB)  + " C", SCR_W/2 + 12 + UI_X_SHIFT, 96, 2, TFT_YELLOW);

    String modeStr = local.fanDisabled ? "MANUAL OFF" : "AUTO MODE";
    String s = "ΔDew=" + (isfinite(local.dewA)&&isfinite(local.dewB)?String(local.dewDelta,1):String("--"))
          + "C  Fan:" + (local.effectiveFan ? "ON" : "OFF") + "  " + modeStr;

    drawCentered(s, SCR_W/2 + UI_X_SHIFT, SCR_H-10, 2, local.effectiveFan ? TFT_RED : TFT_DARKGREEN);

    screen.pushSprite(0,0);

    // Update TFT faster than measurement; looks smooth
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void setup(){
  Serial.begin(115200);
  delay(100);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  // TFT Setup
  // Set rotation(1) for landscape mode
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  screen.createSprite(SCR_W, SCR_H);
  screen.fillSprite(TFT_BLACK);

  // I2C + sensors
  // Set high precision, no heater for both sensors
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire1I2C.begin(I2C_SDA1, I2C_SCL1);
  if (i2cPresent(Wire, SHT4X_ADDR))     {
    hasA=sht4_A.begin(&Wire);
    if(hasA){
      sht4_A.setPrecision(SHT4X_HIGH_PRECISION);
      sht4_A.setHeater(SHT4X_NO_HEATER);
    }
  }
  if (i2cPresent(Wire1I2C, SHT4X_ADDR)) {
    hasB=sht4_B.begin(&Wire1I2C);
    if(hasB){
      sht4_B.setPrecision(SHT4X_HIGH_PRECISION);
      sht4_B.setHeater(SHT4X_NO_HEATER);
    }
  }

  // Initial status for sensors (found or not)
  screen.fillSprite(TFT_BLACK);
  drawCentered(String("SHT4x A: ") + (hasA?"OK":"NOT FOUND"), SCR_W/2, 22, 2, hasA?TFT_GREEN:TFT_ORANGE);
  drawCentered(String("SHT4x B: ") + (hasB?"OK":"NOT FOUND"), SCR_W/2, 42, 2, hasB?TFT_GREEN:TFT_ORANGE);
  screen.pushSprite(0,0); delay(2000);

  // Wi-Fi (STA then AP)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0=millis();

  // Wait up to 20s for WiFi connection
  // if connected => STA mode
  // else => AP mode
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000) delay(250);
  String ipStr, modeStr;
  if (WiFi.status()==WL_CONNECTED) {
    ipStr=WiFi.localIP().toString();
    modeStr="STA";
  }
  else {
    WiFi.mode(WIFI_AP);
    // NOTE: SoftAP password must be >= 8 chars for WPA2. Adjust if needed.
    WiFi.softAP("SHT4x_Monitor","12345678");
    ipStr=WiFi.softAPIP().toString();
    modeStr="AP";
  }

  // Show IP on screen
  screen.fillSprite(TFT_BLACK);
  drawCentered("WiFi " + modeStr, SCR_W/2, 12, 2, TFT_WHITE);
  drawCentered("IP: " + ipStr,    SCR_W/2, 30, 2, TFT_GREEN);
  screen.pushSprite(0,0);
  delay(4000);

  // ---------- LittleFS (mount) ----------
  if (LittleFS.begin(true)) {
    Serial.println("[LittleFS] mounted");
    fs::File root = LittleFS.open("/");
    fs::File file = root.openNextFile();
    while (file) {
      Serial.printf("  %s (%u bytes)\n", file.name(), (unsigned)file.size());
      file = root.openNextFile();
    }
  } else {
    Serial.println("[LittleFS] mount failed");
  }

  // ---------- SD init (hardware only) ----------
  // IMPORTANT: this does NOT create any file yet.
  // SD logging start only after browser opens page and calls /startLogging.
  sdLoggerBegin();

  // CORS for JSON
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // --------- Routes (UI served from LittleFS) ----------
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
  });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
  });
  server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/app.js", "application/javascript; charset=utf-8");
  });
  server.on("/ota.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/ota.html", "text/html; charset=utf-8");
  });
  // serve anything else (favicon, images, css…)
  server.serveStatic("/", LittleFS, "/");
  // ----------------------------------------------------

  // --------- Start SD logging ONLY when page is opened ---------
  
  // /startLogging with body: epoch=SECONDS
  // Browser sends realtime epoch, device sets system time, then create CSV and enable logging.
  server.on("/startLogging", HTTP_POST, [](AsyncWebServerRequest* req){
    if (!req->hasParam("epoch", true)) {
      req->send(400, "application/json", "{\"ok\":0,\"err\":\"missing epoch\"}");
      return;
    }

    time_t epoch = (time_t) req->getParam("epoch", true)->value().toInt();
    if (epoch < 1700000000) {
      req->send(400, "application/json", "{\"ok\":0,\"err\":\"bad epoch\"}");
      return;
    }

    // set device time using browser time
    setTimeFromEpoch(epoch);

    // create/open today's daily.csv now
    if (!sdLoggerStartForToday()) {
      req->send(500, "application/json", "{\"ok\":0,\"err\":\"sd start failed\"}");
      return;
    }

    // turn on logging gate
    sdLoggingEnabled = true;
    req->send(200, "application/json", "{\"ok\":1}");
  });

  // --------- Routes (JSON) --------
  // /data: return JSON with current readings + fan state
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* req){
    SharedState local;
    xSemaphoreTake(g_stateMutex, portMAX_DELAY);
    local = g_state;
    xSemaphoreGive(g_stateMutex);

    String j; j.reserve(256);
    j+="{";
    j+="\"tempA\":"; j+= isfinite(local.tempA)?String(local.tempA,2):"null"; j+=",";
    j+="\"rhA\":";   j+= isfinite(local.rhA)?  String(local.rhA,2)  :"null"; j+=",";
    j+="\"dewA\":";  j+= isfinite(local.dewA)? String(local.dewA,2) :"null"; j+=",";
    j+="\"tempB\":"; j+= isfinite(local.tempB)?String(local.tempB,2):"null"; j+=",";
    j+="\"rhB\":";   j+= isfinite(local.rhB)?  String(local.rhB,2)  :"null"; j+=",";
    j+="\"dewB\":";  j+= isfinite(local.dewB)? String(local.dewB,2) :"null"; j+=",";
    j+="\"fan\":";       j+= (local.effectiveFan?"1":"0"); j+=",";
    j+="\"disabled\":";  j+= (local.fanDisabled ?"1":"0");
    j+="}";

    auto* r = req->beginResponse(200, "application/json", j);
    r->addHeader("Cache-Control","no-store");
    req->send(r);
  });

  // Fan control
  // if "disable=1" => disable fan (manual off)
  // if "disable=0" => enable fan (auto mode)
  // if "toggle"   => toggle disable state
  server.on("/fan", HTTP_ANY, [](AsyncWebServerRequest* req){
    bool newDisabled;

    xSemaphoreTake(g_stateMutex, portMAX_DELAY);
    newDisabled = g_state.fanDisabled;
    xSemaphoreGive(g_stateMutex);

    if (req->hasParam("disable", true))        newDisabled = (req->getParam("disable", true)->value()=="1");
    else if (req->hasParam("toggle", true))    newDisabled = !newDisabled;
    else if (req->hasParam("disable"))         newDisabled = (req->getParam("disable")->value()=="1");
    else if (req->hasParam("toggle"))          newDisabled = !newDisabled;

    // Apply into shared state
    xSemaphoreTake(g_stateMutex, portMAX_DELAY);
    g_state.fanDisabled = newDisabled;
    bool eff = g_state.fanOn && !g_state.fanDisabled;
    g_state.effectiveFan = eff;
    xSemaphoreGive(g_stateMutex);

    digitalWrite(FAN_PIN, eff ? HIGH : LOW);

    String r = String("{\"disabled\":") + (newDisabled? "1":"0") + ",\"fan\":" + (eff? "1":"0") + "}";
    auto* resp = req->beginResponse(200, "application/json", r);
    resp->addHeader("Cache-Control","no-store");
    req->send(resp);
  });

  // --- Reboot route (reliable) ---
  // /reboot -> send JSON, then reboot AFTER client disconnects
  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest* req){
    auto* resp = req->beginResponse(200, "application/json", "{\"ok\":1,\"msg\":\"rebooting\"}");
    resp->addHeader("Cache-Control","no-store");
    req->send(resp);

    // reboot only after TCP connection is closed (most reliable with AsyncWebServer)
    req->onDisconnect([](){
      delay(100);          // short grace period
      ESP.restart();
      delay(1000);         // not reached, but harmless
    });
  });

  // ---------------- SD WEB API (for ZIP download) -----------------

  // List day folders: ["DD-MM-YYYY", ...]
  server.on("/sd/days", HTTP_GET, [](AsyncWebServerRequest* req){
    // SD.h doesn't have great directory listing helpers, but we can open root and iterate
    File root = SD.open("/");
    if (!root) { req->send(500, "application/json", "{\"ok\":0}"); return; }

    String j = "{\"ok\":1,\"days\":[";
    bool first=true;

    File f = root.openNextFile();
    while (f) {
      if (f.isDirectory()) {
        String name = f.name();          // usually "DD-MM-YYYY"
        // remove leading "/" if present
        if (name.startsWith("/")) name = name.substring(1);
        if (!first) j += ",";
        first=false;
        j += "\"" + name + "\"";
      }
      f = root.openNextFile();
    }
    root.close();

    j += "]}";
    req->send(200, "application/json", j);
  });
  // List files inside a day folder
  server.on("/sd/files", HTTP_GET, [](AsyncWebServerRequest* req){
    if (!req->hasParam("day")) {
      req->send(400, "application/json", "{\"ok\":0,\"err\":\"missing day\"}");
      return;
    }
    String day = req->getParam("day")->value();
    String path = "/" + day;

    File dir = SD.open(path.c_str());
    if (!dir) { req->send(404, "application/json", "{\"ok\":0,\"err\":\"no such day\"}"); return; }

    String j = "{\"ok\":1,\"files\":[";
    bool first=true;

    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        String name = f.name();
        // SD sometimes returns full path, keep only file name
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash+1);

        if (!first) j += ",";
        first=false;
        j += "\"" + name + "\"";
      }
      f = dir.openNextFile();
    }
    dir.close();

    j += "]}";
    req->send(200, "application/json", j);
  });
  // Raw file download: /sd/raw?day=DD-MM-YYYY&file=daily.csv
  server.on("/sd/raw", HTTP_GET, [](AsyncWebServerRequest* req){
    if (!req->hasParam("day") || !req->hasParam("file")) {
      req->send(400, "text/plain", "missing day/file");
      return;
    }
    String day  = req->getParam("day")->value();
    String file = req->getParam("file")->value();

    // Basic path safety: block ".."
    if (day.indexOf("..") >= 0 || file.indexOf("..") >= 0) {
      req->send(400, "text/plain", "bad path");
      return;
    }

    String path = "/" + day + "/" + file;
    if (!SD.exists(path.c_str())) {
      req->send(404, "text/plain", "not found");
      return;
    }

    // Stream file
    AsyncWebServerResponse* resp = req->beginResponse(SD, path.c_str(), "text/csv");
    resp->addHeader("Cache-Control", "no-store");
    req->send(resp);
  });
  // Purge warning info for browser banner
  server.on("/sd/status", HTTP_GET, [](AsyncWebServerRequest* req){
    time_t purgeEpoch; int daysLeft; bool warn;
    if (!sdLoggerGetPurgeInfo(purgeEpoch, daysLeft, warn)) {
      req->send(200, "application/json", "{\"ok\":0}");
      return;
    }
    String j = String("{\"ok\":1,\"daysLeft\":") + daysLeft +
              ",\"warn\":" + (warn?"1":"0") +
              ",\"purgeEpoch\":" + (uint32_t)purgeEpoch + "}";
    req->send(200, "application/json", j);
  });

  // --------- OTA (in update.cpp) ---------
  setupOTA(server);

  // -----------------------------------------------
  server.begin();

  // ================= RTOS init =================
  g_stateMutex = xSemaphoreCreateMutex();
  g_sdQueue = xQueueCreate(4, sizeof(SDLogSample)); // small queue is enough

  // Start tasks (pin to cores to keep WiFi stable)
  xTaskCreatePinnedToCore(TaskSensor,  "TaskSensor",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(TaskSD,      "TaskSD",      4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(TaskDisplay, "TaskDisplay", 4096, nullptr, 1, nullptr, 0);
}

/************************************************************************************/

void loop(){
  // Nothing here, everything runs in RTOS tasks
  delay(10);
}