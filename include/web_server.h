#pragma once
// =============================================================
//  WEB SERVER LOKAL (ESPAsyncWebServer)
//
//  Dashboard di LittleFS /www/ (folder data/www/ di project).
//
//  Endpoint                 Method  Fungsi
//  /api/data                GET     snapshot sensor, zona, relay, waktu, koneksi
//  /api/override?mode=N     POST    ganti mode (0 Auto, 1 Manual, 2 Schedule)
//  /api/relay?relay=X       POST    toggle relay (X = main | 1..NUM_ZONES), mode Manual
//  /api/config              GET/POST  konfigurasi umum
//  /api/schedule            GET/POST  jadwal per zona
//  /api/log?limit=N         GET     log historis (JSON)
//  /api/log.csv             GET     unduh log sebagai CSV
//  /api/log/reset           POST    hapus semua log
//  /api/sync-time           POST    set jam dari browser {"epoch": detik}
// =============================================================

void webServerBegin();
