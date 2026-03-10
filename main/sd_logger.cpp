/*
- Write sensor data to microSD as CSV
- IMPORTANT: this module assumes system time is already valid
(main.cpp enforces: only start after browser opens page)

Notes:
- Uses SD library (FAT32) over SPI (SD.h can't handle exFAT))
- This is independent from LittleFS (LittleFS is flash; SD is external)
- Daily logging:
  /DD-MM-YYYY/Data.csv
- Yearly purge:
  At Jan 1 of NEXT year 00:00 (local time), delete all SD contents
  3 days before purge => browser warning
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>

#include "sd_logger.h"

// ===== SD pins (adjust to your wiring) =====
// Can override in main.cpp, otherwise these defaults are used here.
#ifndef SD_SCK
  #define SD_SCK   25
#endif
#ifndef SD_MOSI
  #define SD_MOSI  26
#endif
#ifndef SD_MISO
  #define SD_MISO  27
#endif
#ifndef SD_CS
  #define SD_CS    33
#endif

// -------------------------------
// Internal state (module globals)
// -------------------------------

// g_sdOK: true only if SD.begin() succeeded.
// If false => all writes are skipped (safe no-op).
static bool   g_sdOK = false;

// g_logFile: handle to the currently-open daily CSV file.
static File   g_logFile;

// Current opened day folder (DD-MM-YYYY)
static String g_currentDayFolder = "";

// For periodic purge check throttling
static time_t g_lastPurgeCheck = 0;

// -------------------------------
// Time helpers
// -------------------------------

static bool timeValid() {
  return time(nullptr) > 1700000000; // sanity check
}

static String two(int v) { return (v < 10) ? ("0" + String(v)) : String(v); }

static void getDateStrings(String &dayFolder) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  // Folder name: day-month-year
  dayFolder = two(t.tm_mday) + "-" + two(t.tm_mon + 1) + "-" + String(t.tm_year + 1900);
}

// ISO-like timestamp in CSV: "YYYY-MM-DDTHH:MM:SS"
static String isoNow() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  char buf[32];
  snprintf(buf, sizeof(buf),
           "%04d-%02d-%02dT%02d:%02d:%02d",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

// -------------------------------
// File helpers
// -------------------------------

static void closeLogFile() {
  if (g_logFile) {
    g_logFile.flush();
    g_logFile.close();
  }
}

static bool ensureFolder(const String& folder) {
  String p = "/" + folder;
  if (!SD.exists(p.c_str())) return SD.mkdir(p.c_str());
  return true;
}

static inline String f2(float v) { return isfinite(v) ? String(v,2) : String(""); }

// Open today's /DD-MM-YYYY/daily.csv (create if missing)
static bool openDailyFileForToday() {
  closeLogFile();

  String folder;
  getDateStrings(folder);

  if (!ensureFolder(folder)) {
    Serial.println("[SD] mkdir failed");
    return false;
  }

  g_currentDayFolder = folder;

  String path = "/" + g_currentDayFolder + "/Data.csv";

  // If file does not exist, create and write header
  bool exists = SD.exists(path.c_str());
  g_logFile = SD.open(path.c_str(), FILE_APPEND);
  if (!g_logFile) {
    Serial.println("[SD] open daily.csv failed");
    return false;
  }

  if (!exists) {
    // CSV header (German Excel friendly, you already use ';')
    g_logFile.println("iso_time;tempA;rhA;dewA;tempB;rhB;dewB;deltaDew;fan;disabled");
    g_logFile.flush();
    Serial.printf("[SD] created: %s\n", path.c_str());
  } else {
    Serial.printf("[SD] append: %s\n", path.c_str());
  }

  return true;
}

// Detect day change and reopen file if needed
static void rotateIfNewDay() {
  String folder;
  getDateStrings(folder);
  if (folder != g_currentDayFolder) {
    Serial.printf("[SD] new day detected: %s -> %s\n", g_currentDayFolder.c_str(), folder.c_str());
    openDailyFileForToday();
  }
}

// -------- Purge helpers --------

// Compute next Jan 1 00:00 local time
static time_t nextJan1Local() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  // Next year Jan 1
  t.tm_year = t.tm_year + 1;
  t.tm_mon  = 0;   // Jan
  t.tm_mday = 1;
  t.tm_hour = 0;
  t.tm_min  = 0;
  t.tm_sec  = 0;

  return mktime(&t);
}

// Delete everything recursively from a directory
static void rmTree(const String& path) {
  File dir = SD.open(path.c_str());
  if (!dir) return;

  File f = dir.openNextFile();
  while (f) {
    String full = String(path) + "/" + String(f.name());

    if (f.isDirectory()) {
      f.close();
      rmTree(full);
      SD.rmdir(full.c_str());
    } else {
      f.close();
      SD.remove(full.c_str());
    }
    f = dir.openNextFile();
  }
  dir.close();
}

// Delete all contents under root "/"
static void purgeAllSD() {
  Serial.println("[SD] PURGE START: deleting all SD contents...");

  closeLogFile();
  g_currentDayFolder = "";

  rmTree("/");

  Serial.println("[SD] PURGE DONE");
}

// ======================================================
// Public API
// ======================================================

bool sdLoggerBegin() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("[SD] begin failed");
    g_sdOK = false;
    return false;
  }

  g_sdOK = true;
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("[SD] OK, size: %llu MB\n", (unsigned long long)cardSize);
  return true;
}

bool sdLoggerStartForToday() {
  if (!g_sdOK) return false;
  if (!timeValid()) {
    Serial.println("[SD] time not valid, cannot start daily file yet");
    return false;
  }
  return openDailyFileForToday();
}

void sdLoggerWriteRow(const SDLogSample& s) {
  if (!g_sdOK) return;
  if (!g_logFile) return;
  if (!timeValid()) return;

  // If date changed, switch to new folder + daily.csv automatically
  rotateIfNewDay();

  // iso_time;tempA;rhA;dewA;tempB;rhB;dewB;deltaDew;fan;disabled
  String line;
  line.reserve(160);

  line += isoNow(); line += ";";
  line += f2(s.tempA) + ";" + f2(s.rhA) + ";" + f2(s.dewA) + ";";
  line += f2(s.tempB) + ";" + f2(s.rhB) + ";" + f2(s.dewB) + ";";
  line += f2(s.deltaDew) + ";";
  line += (s.fan ? "1" : "0"); line += ";";
  line += (s.disabled ? "1" : "0");

  g_logFile.println(line);

  // Flush each row is safe for 5-minute sampling (very low wear)
  g_logFile.flush();
}

void sdLoggerEnd() {
  closeLogFile();
}

bool sdLoggerGetPurgeInfo(time_t &purgeEpoch, int &daysLeft, bool &warn) {
  if (!timeValid()) {
    purgeEpoch = 0;
    daysLeft = -1;
    warn = false;
    return false;
  }

  purgeEpoch = nextJan1Local();
  time_t now = time(nullptr);
  time_t diff = purgeEpoch - now;

  // Whole days remaining
  daysLeft = (int)(diff / 86400);

  // Warn if within 3 days before purge AND not past purge
  warn = (diff > 0 && diff <= (3 * 86400));
  return true;
}

void sdLoggerPurgeIfDue() {
  if (!g_sdOK) return;
  if (!timeValid()) return;

  // Throttle checks (once per hour is enough)
  time_t now = time(nullptr);
  if (g_lastPurgeCheck != 0 && (now - g_lastPurgeCheck) < 3600) return;
  g_lastPurgeCheck = now;

  time_t purgeEpoch = nextJan1Local();

  // If now >= purgeEpoch => purge
  if (now >= purgeEpoch) {
    purgeAllSD();
  }
}