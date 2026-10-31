#pragma once
// =============================================================
//  OTA CLOUD — update firmware dari server. Dashboard lokal ikut
//  tertanam di firmware.bin (scripts/embed_www.py), jadi satu update
//  memperbarui keduanya. Isi LittleFS (config, jadwal, log) tidak disentuh.
//
//  otaBegin() dipanggil sekali di setup(); fungsi lainnya hanya dari
//  CloudSyncTask, jadi state modul ini tidak memerlukan mutex.
// =============================================================
#include <Arduino.h>
#include <ArduinoJson.h>

void otaBegin();                         // deteksi update yang gagal (rollback / versi .bin tidak cocok)
void otaConfirmBoot();                   // telemetry sukses: firmware ini dinyatakan valid
void otaOffer(JsonVariantConst offer);   // key "ota" dari balasan server (null = tidak ada / dibatalkan)
void otaService();                       // unduh & pasang jika semua zona idle, restart jika siap
void otaReport(JsonObject telemetry);    // tambahkan status OTA ke telemetry live
