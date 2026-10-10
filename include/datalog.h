#pragma once
// =============================================================
//  LOG HISTORIS
//
//  Menyimpan maksimal MAX_LOG_ENTRIES entri terbaru (circular buffer).
//  Buffer dialokasikan di PSRAM saat boot agar tidak memakan RAM internal,
//  dan disalin ke LittleFS (/logs.bin) setiap ada entri baru.
// =============================================================
#include <Arduino.h>
#include <functional>
#include "types.h"
#include "app_state.h"

bool datalogBegin();                    // alokasi buffer + muat dari file
void datalogAdd(const LogEntry& entry); // tambah entri & simpan ke file
void datalogClear();                    // hapus semua log
size_t datalogCount();

// Panggil `fn` untuk `maxCount` entri terbaru, urut dari yang terlama.
void datalogForEach(size_t maxCount, const std::function<void(const LogEntry&)>& fn);

// Entri dasar dari snapshot state: soil sebelum = sesudah = kondisi sekarang, durasi 0.
LogEntry makeLogEntry(const AppState& state);
