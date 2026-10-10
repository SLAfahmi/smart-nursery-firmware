#pragma once
// =============================================================
//  PENYIMPANAN KONFIGURASI & JADWAL (LittleFS, format JSON)
//
//  File:
//    /config.json   -> Settings
//    /schedule.json -> ScheduleTable
//
//  Fungsi *ToJson / *FromJson juga dipakai Web API dan CloudSyncTask
//  supaya format JSON di semua tempat sama.
// =============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include "types.h"

bool storageBegin();   // mount LittleFS (format otomatis jika belum ada)

void loadSettings(Settings& out);          // nilai default jika file tidak ada / rusak
bool saveSettings(const Settings& s);

void loadSchedule(ScheduleTable& out);
bool saveSchedule(const ScheduleTable& t);

// --- Konversi JSON ---
void settingsToJson(const Settings& s, JsonObject out);
// Update sebagian: hanya key yang ada di JSON yang diubah. Return false + pesan jika tidak valid.
bool settingsFromJson(JsonVariantConst in, Settings& s, String& error);

void scheduleToJson(const ScheduleTable& t, JsonArray out);
bool scheduleFromJson(JsonVariantConst in, ScheduleTable& t, String& error);
