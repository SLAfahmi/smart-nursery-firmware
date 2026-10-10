#pragma once
// =============================================================
//  DAFTAR TASK FreeRTOS
//
//  Task          Core Prio  Fungsi
//  SensorTask      1    5   baca sensor, log berkala, picu FuzzyTask
//  FuzzyTask       1    4   hitung durasi siram (mode Auto)
//  Irrigation1..N  1    3   eksekusi siram satu zona
//  ManualTask      1    1   matikan relay saat timer manual habis
//  ScheduleTask    1    1   picu irigasi sesuai jadwal (mode Schedule)
//  MemoryTask      1    1   laporan heap & stack ke Serial Monitor
//  CloudSyncTask   0    2   telemetry ke server, buffer offline, NTP
//
//  Web server tidak punya task sendiri: ESPAsyncWebServer berjalan di
//  task "async_tcp" milik library (event-driven, tanpa polling).
// =============================================================
#include <Arduino.h>
#include "config.h"

extern TaskHandle_t sensorTaskHandle;
extern TaskHandle_t fuzzyTaskHandle;
extern TaskHandle_t irrigationTaskHandles[NUM_ZONES];

void sensorTask(void* param);
void fuzzyTask(void* param);
void irrigationBegin();                    // panggil sekali sebelum membuat IrrigationTask
void irrigationTask(void* param);          // param = index zona
void manualControlTask(void* param);
void scheduleTask(void* param);
void cloudSyncTask(void* param);
void memoryTask(void* param);

// MemoryTask memantau stack task yang didaftarkan di sini
void memoryWatchTask(TaskHandle_t handle, uint32_t stackSize);

// Status cloud untuk ditampilkan di dashboard
struct CloudStatus {
  bool     lastSendOk;
  uint32_t lastSendUptime;     // uptime (detik) pengiriman sukses terakhir, 0 = belum pernah
  uint16_t offlineQueued;      // jumlah data di buffer offline
};
CloudStatus cloudGetStatus();
