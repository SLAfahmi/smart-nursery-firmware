#pragma once
// =============================================================
//  KONFIGURASI SISTEM
//
//  Semua pin, konstanta, dan nilai default dikumpulkan di file ini.
//  Jika ingin mengubah perilaku alat, cukup ubah nilainya di sini.
// =============================================================
#include <Arduino.h>

// -------------------------------------------------------------
//  1. Pin hardware
// -------------------------------------------------------------
constexpr uint8_t NUM_ZONES = 3;                              // jumlah zona irigasi

constexpr uint8_t SOIL_PINS[NUM_ZONES]       = {5, 4, 2};     // sensor tanah, wajib ADC1 (aman dipakai bersama WiFi)
constexpr uint8_t ZONE_RELAY_PINS[NUM_ZONES] = {14, 15, 7};   // relay valve tiap zona
constexpr uint8_t MAIN_RELAY_PIN             = 20;            // relay pompa utama
constexpr uint8_t DHT_PIN                    = 6;             // DHT22 (suhu & kelembaban udara)
constexpr uint8_t I2C_SDA_PIN                = 8;             // BH1750 + DS3231 berbagi bus I2C
constexpr uint8_t I2C_SCL_PIN                = 9;

constexpr uint8_t RELAY_ON  = LOW;                            // modul relay aktif LOW
constexpr uint8_t RELAY_OFF = HIGH;

// -------------------------------------------------------------
//  2. Kalibrasi sensor kelembaban tanah
//
//  Y      = A + B * adc               (regresi linier terhadap moisture analyzer)
//  soil % = (Y - Y_MIN) * 100 / Y_SPAN, dibatasi 0..100
//  B negatif -> makin kecil ADC makin basah (ADC <= 533 = 100%, ADC >= 2506 = 0%)
// -------------------------------------------------------------
constexpr float   SOIL_REG_A   = 41.7453982f;
constexpr float   SOIL_REG_B   = -0.012665492f;
constexpr float   SOIL_Y_MIN   = 10.0f;
constexpr float   SOIL_Y_SPAN  = 25.0f;
constexpr uint8_t SOIL_SAMPLES = 15;                          // jumlah sampel ADC yang dirata-rata

// Sensor dianggap tidak terpasang (dashboard menampilkan "--%") jika:
constexpr int SOIL_DISCONNECTED_ADC = 200;                    // ADC saat pull-down aktif <= nilai ini (pin kosong)
constexpr int SOIL_ADC_MAX_VALID    = 4000;                   // ADC rata-rata >= nilai ini (mentok 3.3V)

// -------------------------------------------------------------
//  3. Jaringan
// -------------------------------------------------------------
#define AP_SSID "SmartNursery-TelU"                           // nama WiFi hotspot untuk dashboard lokal

// Kredensial (AP_PASSWORD, STA_SSID, STA_PASSWORD, DEVICE_KEY, DASHBOARD_USER,
// DASHBOARD_PASSWORD) disimpan terpisah di secrets.h agar tidak ikut ter-commit.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "include/secrets.h tidak ada: salin include/secrets.example.h menjadi include/secrets.h lalu isi nilainya"
#endif

// Server tujuan (hosting Laravel)
#define CLOUD_BASE_URL   "https://smartnursery-tel-u.srs-ssms.com"
#define CLOUD_URL        CLOUD_BASE_URL "/api/v1/telemetry"
#define CLOUD_BATCH_URL  CLOUD_BASE_URL "/api/v1/telemetry/sync-batch"

constexpr uint32_t CLOUD_SYNC_INTERVAL_MS       = 5000;       // interval kirim telemetry
constexpr uint32_t CLOUD_CONFIRM_DELAY_MS       = 1000;       // kirim ulang cepat setelah menerapkan perintah web
constexpr uint32_t CLOUD_SLOW_REQUEST_MS        = 1500;       // request lebih lama dari ini dicatat di Serial
constexpr uint32_t OFFLINE_SNAPSHOT_INTERVAL_MS = 30000;      // interval simpan snapshot saat offline
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS   = 20000;      // interval mencoba reconnect WiFi STA
constexpr uint16_t HTTP_TIMEOUT_MS              = 5000;

// -------------------------------------------------------------
//  4. OTA cloud (src/ota.cpp)
//
//  Naikkan FIRMWARE_VERSION setiap rilis. Versi yang diisi saat upload
//  firmware.bin di web harus sama persis dengan nilai ini.
// -------------------------------------------------------------
#define FIRMWARE_VERSION "1.0.1"
constexpr uint32_t OTA_RETRY_INTERVAL_MS = 10 * 60 * 1000;    // coba ulang 10 menit setelah update gagal
constexpr uint16_t OTA_HTTP_TIMEOUT_MS   = 15000;             // unduhan dianggap putus jika tidak ada data selama ini

// -------------------------------------------------------------
//  5. Waktu (RTC DS3231 menyimpan waktu lokal WIB)
// -------------------------------------------------------------
#define TZ_INFO "WIB-7"                                       // format POSIX TZ untuk UTC+7
constexpr int32_t TZ_OFFSET_SEC = 7 * 3600;
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"

// -------------------------------------------------------------
//  6. Perilaku irigasi
// -------------------------------------------------------------
constexpr uint8_t  MAX_CONCURRENT_IRRIGATION = NUM_ZONES;     // batas zona yang boleh menyiram bersamaan
constexpr uint16_t SOAK_DELAY_SEC            = 15;            // jeda agar air meresap setelah siram
constexpr float    SCHEDULE_SOIL_MAX         = 60.0f;         // jadwal dilewati jika tanah > nilai ini (%)
constexpr uint8_t  MAX_SCHEDULES             = 3;             // jumlah slot jadwal per zona

// -------------------------------------------------------------
//  7. Penyimpanan
// -------------------------------------------------------------
constexpr uint16_t MAX_LOG_ENTRIES     = 100;                 // jumlah log historis yang disimpan
constexpr uint16_t MAX_OFFLINE_ENTRIES = 50;                  // jumlah data offline yang menunggu dikirim

// -------------------------------------------------------------
//  8. Nilai default konfigurasi (bisa diubah dari dashboard)
// -------------------------------------------------------------
constexpr uint16_t DEFAULT_SENSOR_INTERVAL_SEC = 10;          // baca sensor (tanah, DHT22, BH1750) tiap 10 detik
constexpr uint16_t DEFAULT_MANUAL_DURATION_SEC = 60;          // durasi timer mode Manual
constexpr uint32_t DEFAULT_LOG_INTERVAL_SEC    = 3600;        // catat log (CSV) tiap 1 jam
constexpr uint32_t DEFAULT_COOLDOWN_SEC        = 4 * 3600;    // jeda minimal antar penyiraman otomatis: 4 jam

// -------------------------------------------------------------
//  9. Monitoring memori (hanya ke Serial Monitor)
// -------------------------------------------------------------
constexpr uint32_t MEMORY_REPORT_INTERVAL_MS = 10000;
constexpr uint32_t STACK_WARNING_BYTES       = 512;           // peringatan jika sisa stack < nilai ini
