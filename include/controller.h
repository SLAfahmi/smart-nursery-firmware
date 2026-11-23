#pragma once
// =============================================================
//  CONTROLLER — aksi tingkat sistem
//
//  Dipanggil dari Web API, CloudSyncTask, dan task lain. Semua aturan
//  "siapa boleh menyalakan relay kapan" dikumpulkan di sini.
// =============================================================
#include <Arduino.h>
#include "types.h"

// Nomor relay untuk kontrol manual:
//   0..NUM_ZONES-1 = valve zona, MAIN_RELAY_ID = pompa utama
constexpr int MAIN_RELAY_ID = -1;

// true jika nomor relay dikenal (pompa utama atau salah satu zona)
bool isValidRelayId(int relay);

// Ganti mode: semua relay OFF, zona kembali Idle, irigasi yang berjalan dibatalkan.
void setSystemMode(SystemMode mode);

// Minta zona disiram (dari FuzzyTask / ScheduleTask).
// Return false jika zona sedang sibuk.
bool requestIrrigation(uint8_t zone, uint16_t durationSec, TriggerSource source);

// Kontrol relay pada mode Manual
bool manualToggle(int relay, String& error);   // balik kondisi ON/OFF
void manualSet(int relay, bool on);            // set ON/OFF langsung

// Simpan konfigurasi ke state + LittleFS
void applySettings(const Settings& s);
void applySchedule(const ScheduleTable& t);
