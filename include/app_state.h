#pragma once
// =============================================================
//  STATE BERSAMA ANTAR TASK
//
//  Semua data yang dibaca/ditulis lebih dari satu task disimpan di
//  variabel global `app`. Aksesnya WAJIB diapit lockState()/unlockState().
//
//  Aturan agar sistem tetap responsif:
//    - Jangan melakukan I/O lambat (I2C, file, jaringan, Serial) selama
//      memegang lock. Salin data dulu, lepas lock, baru proses.
// =============================================================
#include <Arduino.h>
#include "config.h"
#include "types.h"

struct AppState {
  SystemMode     mode = SystemMode::Auto;
  SensorReadings sensors;
  Zone           zones[NUM_ZONES];
  bool           mainRelayOn      = false;
  uint32_t       mainRelayOnSince = 0;       // millis() saat relay utama dinyalakan
  Settings       settings;
  ScheduleTable  schedule;
};

extern AppState app;

// Mutex untuk struct `app`
void lockState();
void unlockState();

// Salinan seluruh state (untuk dibaca tanpa menahan lock lama-lama)
AppState copyState();

// Mutex bus I2C — BH1750 dan RTC DS3231 berbagi bus yang sama dan diakses
// dari task berbeda. Tanpa mutex, transaksi I2C bisa bertabrakan.
bool lockI2C(uint32_t timeoutMs = 200);
void unlockI2C();

void appStateInit();

// Detik sejak boot (monotonik, tidak terpengaruh perubahan jam RTC)
uint32_t uptimeSec();
