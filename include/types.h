#pragma once
// =============================================================
//  TIPE DATA BERSAMA
// =============================================================
#include <Arduino.h>
#include "config.h"

enum class SystemMode : uint8_t { Auto = 0, Manual = 1, Schedule = 2 };

enum class ManualType : uint8_t { Toggle = 0, Timer = 1 };

// Siklus hidup satu zona saat irigasi otomatis/terjadwal:
// Idle -> Queued -> Watering -> Soaking -> Idle
enum class ZoneState : uint8_t { Idle, Queued, Watering, Soaking };

enum class TriggerSource : uint8_t { None, Fuzzy, Schedule, Manual };

struct SensorReadings {
  float temperature = NAN;   // °C
  float humidity    = NAN;   // %
  float lux         = NAN;   // lx
  bool  dhtOk       = false;
  bool  luxOk       = false;
};

struct Zone {
  float         soil         = 0;                  // % kelembaban tanah terkini
  float         soilBefore   = 0;                  // % saat penyiraman dimulai
  float         fuzzyOutput  = 0;                  // rekomendasi fuzzy terakhir (detik)
  ZoneState     state        = ZoneState::Idle;
  TriggerSource source       = TriggerSource::None;
  uint16_t      durationSec  = 0;                  // durasi siram yang diminta
  bool          relayOn      = false;
  uint32_t      relayOnSince = 0;                  // millis() saat relay dinyalakan
  uint32_t      lastWateredUptime = 0;             // uptime (detik) selesai siram, 0 = belum pernah
};

// Konfigurasi umum yang bisa diubah dari dashboard / cloud
struct Settings {
  uint16_t   sensorIntervalSec = DEFAULT_SENSOR_INTERVAL_SEC;
  ManualType manualType        = ManualType::Toggle;
  uint16_t   manualDurationSec = DEFAULT_MANUAL_DURATION_SEC;
  uint32_t   logIntervalSec    = DEFAULT_LOG_INTERVAL_SEC;
  uint32_t   cooldownSec       = DEFAULT_COOLDOWN_SEC;

  bool operator==(const Settings& o) const {
    return sensorIntervalSec == o.sensorIntervalSec && manualType == o.manualType &&
           manualDurationSec == o.manualDurationSec && logIntervalSec == o.logIntervalSec &&
           cooldownSec == o.cooldownSec;
  }
  bool operator!=(const Settings& o) const { return !(*this == o); }
};

struct ScheduleSlot {
  bool     active      = false;
  uint8_t  hour        = 6;
  uint8_t  minute      = 0;
  uint16_t durationSec = 60;

  bool operator==(const ScheduleSlot& o) const {
    return active == o.active && hour == o.hour && minute == o.minute && durationSec == o.durationSec;
  }
  bool operator!=(const ScheduleSlot& o) const { return !(*this == o); }
};

// Jadwal semua zona: slot[zona][nomor slot]
struct ScheduleTable {
  ScheduleSlot slot[NUM_ZONES][MAX_SCHEDULES];

  bool operator==(const ScheduleTable& o) const {
    for (uint8_t z = 0; z < NUM_ZONES; z++)
      for (uint8_t s = 0; s < MAX_SCHEDULES; s++)
        if (slot[z][s] != o.slot[z][s]) return false;
    return true;
  }
  bool operator!=(const ScheduleTable& o) const { return !(*this == o); }
};

// Satu baris log historis (disimpan di LittleFS, diekspor sebagai CSV)
struct LogEntry {
  uint32_t timestamp;                  // epoch UTC, 0 jika waktu belum valid
  float    temperature;
  float    humidity;
  float    lux;
  float    soilBefore[NUM_ZONES];
  float    soilAfter[NUM_ZONES];
  uint16_t durationSec[NUM_ZONES];
  uint8_t  systemMode;
};

const char* toString(SystemMode mode);
const char* toString(ZoneState state);
const char* toString(TriggerSource source);
