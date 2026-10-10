#pragma once
// =============================================================
//  CONTROLLER — aksi tingkat sistem
//
//  Dipanggil dari Web API, CloudSyncTask, dan task lain. Semua aturan
//  "siapa boleh menyalakan relay kapan" dikumpulkan di sini.
// =============================================================
#include <Arduino.h>
#include "types.h"

// Ganti mode: semua relay OFF, zona kembali Idle, irigasi yang berjalan dibatalkan.
void setSystemMode(SystemMode mode);

// Minta zona disiram (dari FuzzyTask / ScheduleTask).
// Return false jika zona sedang sibuk.
bool requestIrrigation(uint8_t zone, uint16_t durationSec, TriggerSource source);

// Mode manual. relay: 0..NUM_ZONES-1 = valve zona, MAIN_RELAY_ID = pompa utama.
constexpr int MAIN_RELAY_ID = -1;
bool manualToggle(int relay, String& error);
void manualSet(int relay, bool on);

// Simpan konfigurasi ke state + LittleFS
void applySettings(const Settings& s);
void applySchedule(const ScheduleTable& t);
