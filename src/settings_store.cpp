#include "settings_store.h"
#include <LittleFS.h>

static const char* SETTINGS_FILE = "/config.json";
static const char* SCHEDULE_FILE = "/schedule.json";

// ---------- Helper file JSON ----------

static bool readJsonFile(const char* path, JsonDocument& doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[STORAGE] %s rusak (%s), pakai default\n", path, err.c_str());
    return false;
  }
  return true;
}

static bool writeJsonFile(const char* path, const JsonDocument& doc) {
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("[STORAGE] Gagal membuka %s untuk ditulis\n", path);
    return false;
  }
  serializeJson(doc, f);
  f.close();
  return true;
}

// Ambil angka dari JSON jika ada; cek rentangnya.
// Return false (dan isi error) hanya jika key ada tetapi nilainya tidak valid.
template <typename T>
static bool readNumber(JsonVariantConst in, const char* key, long minVal, long maxVal,
                       T& target, String& error) {
  JsonVariantConst v = in[key];
  if (v.isNull()) return true;                       // key tidak dikirim -> biarkan
  if (!v.is<long>()) {
    error = String(key) + " harus berupa angka";
    return false;
  }
  long value = v.as<long>();
  if (value < minVal || value > maxVal) {
    error = String(key) + " harus " + minVal + ".." + maxVal;
    return false;
  }
  target = (T)value;
  return true;
}

// ---------- LittleFS ----------

bool storageBegin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[STORAGE] Gagal mount LittleFS!");
    return false;
  }
  Serial.printf("[STORAGE] LittleFS OK: %u / %u KB terpakai\n",
                (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  return true;
}

// ---------- Settings ----------

void settingsToJson(const Settings& s, JsonObject out) {
  out["sensorInterval"] = s.sensorIntervalSec;
  out["manualMode"]     = (uint8_t)s.manualType;
  out["manualDuration"] = s.manualDurationSec;
  out["logInterval"]    = s.logIntervalSec;
  out["cooldown"]       = s.cooldownSec;
}

bool settingsFromJson(JsonVariantConst in, Settings& s, String& error) {
  Settings result = s;
  uint8_t manualType = (uint8_t)result.manualType;

  if (!readNumber(in, "sensorInterval", 2, 3600, result.sensorIntervalSec, error)) return false;
  if (!readNumber(in, "manualMode", 0, 1, manualType, error)) return false;
  if (!readNumber(in, "manualDuration", 1, 3600, result.manualDurationSec, error)) return false;
  if (!readNumber(in, "logInterval", 60, 86400, result.logIntervalSec, error)) return false;
  if (!readNumber(in, "cooldown", 0, 86400, result.cooldownSec, error)) return false;

  result.manualType = (ManualType)manualType;
  s = result;
  return true;
}

void loadSettings(Settings& out) {
  out = Settings();   // default
  JsonDocument doc;
  if (!readJsonFile(SETTINGS_FILE, doc)) return;

  String error;
  if (!settingsFromJson(doc.as<JsonVariantConst>(), out, error)) {
    Serial.printf("[STORAGE] config.json tidak valid (%s), pakai default\n", error.c_str());
    out = Settings();
  }
}

bool saveSettings(const Settings& s) {
  JsonDocument doc;
  settingsToJson(s, doc.to<JsonObject>());
  return writeJsonFile(SETTINGS_FILE, doc);
}

// ---------- Jadwal ----------
// Format JSON:
// [ { "zone": 1, "slots": [ {"active":true,"hour":6,"minute":0,"duration":60}, ... ] }, ... ]

void scheduleToJson(const ScheduleTable& t, JsonArray out) {
  for (uint8_t z = 0; z < NUM_ZONES; z++) {
    JsonObject zone = out.add<JsonObject>();
    zone["zone"] = z + 1;
    JsonArray slots = zone["slots"].to<JsonArray>();
    for (uint8_t s = 0; s < MAX_SCHEDULES; s++) {
      const ScheduleSlot& slot = t.slot[z][s];
      JsonObject o = slots.add<JsonObject>();
      o["active"]   = slot.active;
      o["hour"]     = slot.hour;
      o["minute"]   = slot.minute;
      o["duration"] = slot.durationSec;
    }
  }
}

bool scheduleFromJson(JsonVariantConst in, ScheduleTable& t, String& error) {
  JsonArrayConst zones = in.as<JsonArrayConst>();
  if (zones.isNull()) {
    error = "jadwal harus berupa array per zona";
    return false;
  }

  ScheduleTable result = t;
  uint8_t z = 0;
  for (JsonVariantConst zone : zones) {
    if (z >= NUM_ZONES) break;
    uint8_t s = 0;
    for (JsonVariantConst slotJson : zone["slots"].as<JsonArrayConst>()) {
      if (s >= MAX_SCHEDULES) break;
      ScheduleSlot& slot = result.slot[z][s];
      slot.active = slotJson["active"] | false;
      if (!readNumber(slotJson, "hour", 0, 23, slot.hour, error)) return false;
      if (!readNumber(slotJson, "minute", 0, 59, slot.minute, error)) return false;
      if (!readNumber(slotJson, "duration", 1, 3600, slot.durationSec, error)) return false;
      s++;
    }
    z++;
  }

  t = result;
  return true;
}

void loadSchedule(ScheduleTable& out) {
  out = ScheduleTable();
  JsonDocument doc;
  if (!readJsonFile(SCHEDULE_FILE, doc)) return;

  String error;
  if (!scheduleFromJson(doc.as<JsonVariantConst>(), out, error)) {
    Serial.printf("[STORAGE] schedule.json tidak valid (%s), pakai default\n", error.c_str());
    out = ScheduleTable();
  }
}

bool saveSchedule(const ScheduleTable& t) {
  JsonDocument doc;
  scheduleToJson(t, doc.to<JsonArray>());
  return writeJsonFile(SCHEDULE_FILE, doc);
}
