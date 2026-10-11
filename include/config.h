#pragma once
// =============================================================
//  KONFIGURASI SISTEM — semua konstanta yang boleh diubah ada di sini
// =============================================================
#include <Arduino.h>

// -------------------------------------------------------------
//  Hardware: pin
// -------------------------------------------------------------
constexpr uint8_t NUM_ZONES = 3;

constexpr uint8_t SOIL_PINS[NUM_ZONES]       = {1, 2, 3};    // ADC1 (aman dipakai bersama WiFi)
constexpr uint8_t ZONE_RELAY_PINS[NUM_ZONES] = {14, 15, 7};
constexpr uint8_t MAIN_RELAY_PIN             = 20;           // relay pompa utama
constexpr uint8_t DHT_PIN                    = 6;
constexpr uint8_t I2C_SDA_PIN                = 8;            // BH1750 + DS3231 berbagi bus
constexpr uint8_t I2C_SCL_PIN                = 9;

constexpr uint8_t RELAY_ON  = LOW;                           // modul relay active LOW
constexpr uint8_t RELAY_OFF = HIGH;

// -------------------------------------------------------------
//  Kalibrasi sensor kelembaban tanah
//  Y      = A + B * adc               (regresi linier terhadap moisture analyzer)
//  soil % = (Y - Y_MIN) * 100 / Y_SPAN, dibatasi 0..100
//  B negatif -> makin kecil ADC makin basah (ADC <= 533 = 100%, ADC >= 2506 = 0%)
// -------------------------------------------------------------
constexpr float   SOIL_REG_A    = 41.7453982f;
constexpr float   SOIL_REG_B    = -0.012665492f;
constexpr float   SOIL_Y_MIN    = 10.0f;
constexpr float   SOIL_Y_SPAN   = 25.0f;
constexpr uint8_t SOIL_SAMPLES  = 15;                         // rata-rata N kali baca ADC
// Sensor dianggap tidak terpasang (tampil "--%") jika:
constexpr int     SOIL_DISCONNECTED_ADC = 200;                // ADC saat pull-down aktif <= ini (pin kosong)
constexpr int     SOIL_ADC_MAX_VALID    = 4000;               // ADC rata-rata >= ini (mentok 3.3V)

// -------------------------------------------------------------
//  Jaringan
// -------------------------------------------------------------
#define AP_SSID        "SmartNursery-TelU"
#define AP_PASSWORD    "12345678"                          // minimal 8 karakter
#define STA_SSID       "IT_SRS"
#define STA_PASSWORD   "ISI_PASSWORD_WIFI"

// Server tujuan. Untuk uji lokal (laptop menjalankan `php artisan serve --host=0.0.0.0`),
// aktifkan USE_LOCAL_SERVER dan isi IP laptop. Laptop & ESP harus di WiFi yang sama.
// Matikan lagi sebelum upload firmware final.
// #define USE_LOCAL_SERVER
#ifdef USE_LOCAL_SERVER
  #define CLOUD_BASE_URL "http://192.168.43.10:8000"          // IP laptop (cek dengan ipconfig)
#else
  #define CLOUD_BASE_URL "https://smartnursery-tel-u.srs-ssms.com"
#endif
#define CLOUD_URL        CLOUD_BASE_URL "/api/v1/telemetry"
#define CLOUD_BATCH_URL  CLOUD_BASE_URL "/api/v1/telemetry/sync-batch"
#define DEVICE_KEY       "ISI_DEVICE_KEY"                    // harus sama dengan DEVICE_TOKEN di .env server

constexpr uint32_t CLOUD_SYNC_INTERVAL_MS     = 5000;
constexpr uint32_t OFFLINE_SNAPSHOT_INTERVAL_MS = 30000;     // simpan snapshot saat STA putus
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 20000;
constexpr uint16_t HTTP_TIMEOUT_MS            = 5000;

// -------------------------------------------------------------
//  Waktu (RTC DS3231 menyimpan waktu lokal WIB)
// -------------------------------------------------------------
#define TZ_INFO        "WIB-7"                               // POSIX TZ untuk UTC+7
constexpr int32_t TZ_OFFSET_SEC = 7 * 3600;
#define NTP_SERVER_1   "pool.ntp.org"
#define NTP_SERVER_2   "time.google.com"

// -------------------------------------------------------------
//  Perilaku irigasi
// -------------------------------------------------------------
constexpr uint8_t  MAX_CONCURRENT_IRRIGATION = NUM_ZONES;    // batas zona menyiram bersamaan
constexpr uint16_t SOAK_DELAY_SEC            = 15;           // jeda serap air setelah siram
constexpr float    SCHEDULE_SOIL_MAX         = 60.0f;        // jadwal dilewati jika tanah > nilai ini
constexpr uint8_t  MAX_SCHEDULES             = 3;            // slot jadwal per zona

// -------------------------------------------------------------
//  Penyimpanan
// -------------------------------------------------------------
constexpr uint16_t MAX_LOG_ENTRIES     = 100;
constexpr uint16_t MAX_OFFLINE_ENTRIES = 50;

// -------------------------------------------------------------
//  Nilai default konfigurasi (bisa diubah dari dashboard)
// -------------------------------------------------------------
constexpr uint16_t DEFAULT_SENSOR_INTERVAL_SEC = 10;
constexpr uint16_t DEFAULT_MANUAL_DURATION_SEC = 60;
constexpr uint32_t DEFAULT_LOG_INTERVAL_SEC    = 3600;       // 1 jam
constexpr uint32_t DEFAULT_COOLDOWN_SEC        = 4 * 3600;   // 4 jam

// -------------------------------------------------------------
//  Monitoring memori (hanya Serial Monitor)
// -------------------------------------------------------------
constexpr uint32_t MEMORY_REPORT_INTERVAL_MS = 10000;
constexpr uint32_t STACK_WARNING_BYTES       = 512;          // peringatan jika sisa stack < ini
