#pragma once
#include <Arduino.h>

/*
  SD logger (SPI microSD):
  - Folder per day: DD-MM-YYYY/
  - One CSV per day (5 minutes for 1 sample)
  - File name: Data.csv
  - Yearly purge on Jan 1 of next year 00:00
  - 3 days before purge => browser warning via sdLoggerGetPurgeInfo()
  
*/

struct SDLogSample {
  float tempA, rhA, dewA;
  float tempB, rhB, dewB;
  float deltaDew;
  bool  fan;       // effective fan (after disabled)
  bool  disabled;  // fanDisabled
};

bool sdLoggerBegin();                         // init SD (hardware only)
bool sdLoggerStartForToday();                 // open/create today's folder + daily.csv (needs valid time)
void sdLoggerWriteRow(const SDLogSample& s);  // write one line
void sdLoggerEnd();                           // close file (optional)

// Purge info for browser warning (3 days before purge) 
//   purgeEpoch: epoch time for next Jan 1 00:00 local time
//   daysLeft:   whole days remaining until purge (>=0)
//   warn:       true if within 3 days before purge
bool sdLoggerGetPurgeInfo(time_t &purgeEpoch, int &daysLeft, bool &warn);

// Called occasionally (e.g., every hour) to check and purge when time reached
void sdLoggerPurgeIfDue();
