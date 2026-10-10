#pragma once
// =============================================================
//  JAM SISTEM
//
//  Semua task membaca waktu dari jam internal ESP32 (time()), BUKAN
//  langsung dari RTC. RTC DS3231 hanya dipakai untuk:
//    1. mengisi jam internal saat boot,
//    2. menyimpan waktu baru saat sync dari browser / NTP.
//  Dengan begitu bus I2C tidak dibebani setiap kali butuh waktu, dan
//  sistem tetap punya jam walaupun modul RTC bermasalah.
// =============================================================
#include <Arduino.h>
#include <time.h>

enum class TimeSource : uint8_t { None, Rtc, Browser, Ntp };

void clockBegin();                 // panggil setelah Wire.begin()

bool   clockIsValid();             // false jika jam belum pernah di-set
time_t clockNow();                 // epoch UTC
bool   clockLocal(struct tm& out); // waktu lokal (WIB)
String clockFormat(time_t epoch);  // "YYYY-MM-DD HH:MM:SS" (WIB)

bool       clockRtcOk();           // RTC terdeteksi di bus I2C
TimeSource clockSource();

// Set jam dari browser. Return true jika RTC juga berhasil ditulis.
bool clockSetFromBrowser(time_t epoch);

// Dipanggil berkala dari CloudSyncTask saat WiFi STA terhubung:
// memulai NTP sekali, lalu menyalin hasil NTP ke RTC.
void clockServiceNtp();

const char* toString(TimeSource source);
