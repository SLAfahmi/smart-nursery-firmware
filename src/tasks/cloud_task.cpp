// =============================================================
//  CloudSyncTask (Core 0) — sinkronisasi dengan server Laravel
//
//  Saat WiFi STA terhubung:
//    - kirim telemetry ke CLOUD_URL setiap CLOUD_SYNC_INTERVAL_MS
//    - jika sukses: terapkan `device_command` dari server, kirim isi
//      buffer offline (batch) ke CLOUD_BATCH_URL, lalu jalankan OTA
//      jika server menawarkan firmware baru (src/ota.cpp)
//    - jika gagal : simpan payload ke buffer offline
//    - jalankan NTP & salin waktunya ke RTC
//  Saat WiFi STA putus:
//    - simpan snapshot ke buffer offline setiap OFFLINE_SNAPSHOT_INTERVAL_MS
//    - coba reconnect setiap WIFI_RECONNECT_INTERVAL_MS
//
//  Buffer offline: /offline_q.jsonl (satu payload JSON per baris),
//  maksimal MAX_OFFLINE_ENTRIES baris, yang terlama dibuang.
// =============================================================
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "tasks.h"
#include "app_state.h"
#include "controller.h"
#include "settings_store.h"
#include "clock.h"
#include "ota.h"

static const char* OFFLINE_FILE = "/offline_q.jsonl";

// --- Status untuk dashboard (dibaca web server lewat cloudGetStatus) ---
static volatile bool     lastSendOk     = false;
static volatile uint32_t lastSendUptime = 0;
static volatile uint16_t offlineCount   = 0;

// Versi device_command terakhir yang diterapkan (0 setelah boot). Server mengirim
// konfigurasi penuh selama versi ini berbeda dengan versi di web.
static uint32_t appliedCommandVersion = 0;

// --- Pengatur waktu di dalam task (millis) ---
static uint32_t lastSendMs      = 0;
static uint32_t lastSnapshotMs  = 0;
static uint32_t lastReconnectMs = 0;
static bool     quickConfirm    = false;   // kirim berikutnya lebih cepat (setelah ada perintah web)

CloudStatus cloudGetStatus() {
  return {lastSendOk, lastSendUptime, offlineCount};
}

// =============================================================
//  Payload telemetry
// =============================================================

static void addZonesTelemetry(JsonArray zones, const AppState& s) {
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    JsonObject z = zones.add<JsonObject>();
    z["zone"]           = i + 1;
    z["soil"]           = s.zones[i].soil;
    z["relay"]          = s.zones[i].relayOn;
    z["state"]          = toString(s.zones[i].state);
    z["fuzzy_duration"] = s.zones[i].fuzzyOutput;
  }
}

// Versi perintah, konfigurasi & jadwal yang sedang berlaku (agar perubahan dari
// dashboard lokal ikut tampil di web), versi firmware, dan status OTA
static void addConfigTelemetry(JsonDocument& doc, const AppState& s) {
  doc["command_version"]  = appliedCommandVersion;
  doc["firmware_version"] = FIRMWARE_VERSION;
  settingsToJson(s.settings, doc["config"].to<JsonObject>());
  scheduleToJson(s.schedule, doc["schedules"].to<JsonArray>());
  otaReport(doc.as<JsonObject>());
}

// includeConfig = false untuk buffer offline (data lama, dan memperbesar ukuran batch)
static String buildTelemetry(bool includeConfig) {
  AppState s = copyState();

  JsonDocument doc;
  if (clockIsValid()) doc["recorded_at"] = clockFormat(clockNow());   // waktu WIB
  else                doc["recorded_at"] = nullptr;
  doc["temperature"] = s.sensors.temperature;   // NaN otomatis jadi null
  doc["humidity"]    = s.sensors.humidity;
  doc["light"]       = s.sensors.lux;
  doc["system_mode"] = (uint8_t)s.mode;
  doc["relay_main"]  = s.mainRelayOn;
  addZonesTelemetry(doc["zones"].to<JsonArray>(), s);

  if (includeConfig) addConfigTelemetry(doc, s);

  String out;
  serializeJson(doc, out);
  return out;
}

// =============================================================
//  HTTP
// =============================================================

// Kirim POST JSON ke server.
// Return kode HTTP (> 0) atau kode error HTTPClient (< 0).
static int httpPost(const char* url, const String& body, String* response) {
  WiFiClientSecure secureClient;
  WiFiClient       plainClient;
  bool isHttps = strncmp(url, "https", 5) == 0;
  if (isHttps) secureClient.setInsecure();   // tanpa verifikasi sertifikat
  WiFiClient& client = isHttps ? static_cast<WiFiClient&>(secureClient) : plainClient;

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return -1;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("X-Device-Token", DEVICE_KEY);

  int code = http.POST(body);
  if (response && code > 0) *response = http.getString();
  http.end();
  return code;
}

static bool isSuccess(int httpCode) { return httpCode >= 200 && httpCode < 300; }

// =============================================================
//  Perintah dari server (device_command)
//  { "device_command": { "version": n, "system_mode": 0..2,
//                        "manual_mode_type": 0..1, "manual_duration": detik,
//                        "sensor_interval": detik, "log_interval": detik,
//                        "cooldown": detik, "schedules": [...],
//                        "relays": [{"relay": "main"|1..N, "on": bool}] } }
//  Semua key opsional. Format "schedules" sama dengan GET /api/schedule.
// =============================================================

// Pasangan nama key pengaturan: versi cloud -> versi lokal (/api/config)
struct SettingKeyMap {
  const char* cloudKey;
  const char* localKey;
};

static const SettingKeyMap SETTING_KEYS[] = {
  {"manual_mode_type", "manualMode"},
  {"manual_duration",  "manualDuration"},
  {"sensor_interval",  "sensorInterval"},
  {"log_interval",     "logInterval"},
  {"cooldown",         "cooldown"},
};

// Ubah id relay dari server ("main" atau nomor zona 1..N) menjadi nomor relay lokal.
// Return false jika id tidak dikenal.
static bool parseRelayId(JsonVariantConst id, int& relay) {
  if (id.is<const char*>() && strcmp(id.as<const char*>(), "main") == 0) relay = MAIN_RELAY_ID;
  else if (id.is<int>())                                                 relay = id.as<int>() - 1;   // zona 1-based -> index
  else return false;

  return isValidRelayId(relay);
}

// 1. Mode sistem
static void applyModeCommand(JsonVariantConst cmd) {
  if (!cmd["system_mode"].is<int>()) return;
  int mode = cmd["system_mode"];
  if (mode >= 0 && mode <= 2) setSystemMode((SystemMode)mode);
}

// 2. Pengaturan umum
static void applySettingsCommand(JsonVariantConst cmd) {
  // Salin key yang dikirim server ke format lokal, supaya bisa divalidasi settingsFromJson()
  JsonDocument local;
  for (const SettingKeyMap& key : SETTING_KEYS) {
    if (!cmd[key.cloudKey].isNull()) local[key.localKey] = cmd[key.cloudKey];
  }
  if (local.size() == 0) return;

  Settings current = copyState().settings;
  Settings updated = current;
  String   error;
  if (!settingsFromJson(local.as<JsonVariantConst>(), updated, error)) {
    Serial.printf("[CLOUD] device_command pengaturan ditolak: %s\n", error.c_str());
  } else if (updated != current) {
    applySettings(updated);
    Serial.println("[CLOUD] Pengaturan diperbarui dari server");
  }
}

// 3. Jadwal
static void applyScheduleCommand(JsonVariantConst cmd) {
  if (cmd["schedules"].isNull()) return;

  ScheduleTable current = copyState().schedule;
  ScheduleTable updated = current;
  String        error;
  if (!scheduleFromJson(cmd["schedules"], updated, error)) {
    Serial.printf("[CLOUD] device_command jadwal ditolak: %s\n", error.c_str());
  } else if (updated != current) {
    applySchedule(updated);
    Serial.println("[CLOUD] Jadwal diperbarui dari server");
  }
}

// 4. Relay (hanya dijalankan pada mode Manual)
static void applyRelayCommands(JsonVariantConst cmd) {
  if (!cmd["relays"].is<JsonArrayConst>()) return;

  for (JsonVariantConst r : cmd["relays"].as<JsonArrayConst>()) {
    int relay;
    if (!parseRelayId(r["relay"], relay)) continue;
    if (!r["on"].is<bool>()) continue;

    if (copyState().mode != SystemMode::Manual) {
      Serial.println("[CLOUD] Perintah relay diabaikan: bukan mode Manual");
      return;
    }
    manualSet(relay, r["on"].as<bool>());
  }
}

// 5. Catat versi perintah yang sudah diterapkan
static void saveCommandVersion(JsonVariantConst cmd) {
  if (!cmd["version"].is<uint32_t>()) return;

  uint32_t version = cmd["version"];
  if (version != appliedCommandVersion) {
    appliedCommandVersion = version;
    Serial.printf("[CLOUD] Konfigurasi versi %lu diterapkan\n", (unsigned long)version);
  }
}

static void applyDeviceCommand(JsonVariantConst cmd) {
  if (cmd.isNull()) return;

  applyModeCommand(cmd);
  applySettingsCommand(cmd);
  applyScheduleCommand(cmd);
  applyRelayCommands(cmd);     // setelah mode, agar "ganti ke Manual + nyalakan relay" bisa dalam satu perintah
  saveCommandVersion(cmd);
}

// Proses balasan server. Return true jika server mengirim device_command.
static bool handleServerResponse(const String& response) {
  if (response.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;   // bukan JSON, abaikan

  // Tawaran firmware ada di luar device_command, agar tidak memicu quickConfirm
  otaOffer(doc["ota"]);

  JsonVariantConst cmd = doc["device_command"];
  if (cmd.isNull()) cmd = doc["data"]["device_command"];
  applyDeviceCommand(cmd);
  return !cmd.isNull();
}

// =============================================================
//  Buffer offline (LittleFS)
// =============================================================

// Hitung jumlah baris data di file buffer (dipanggil sekali saat task mulai)
static void countOfflineEntries() {
  offlineCount = 0;
  File f = LittleFS.open(OFFLINE_FILE, "r");
  if (!f) return;
  while (f.available()) {
    if (f.readStringUntil('\n').length() > 1) offlineCount++;
  }
  f.close();
}

// Buang baris pertama (data terlama) dari file buffer
static void dropOldestOffline() {
  File f = LittleFS.open(OFFLINE_FILE, "r");
  if (!f) return;
  f.readStringUntil('\n');
  String rest = f.readString();
  f.close();

  f = LittleFS.open(OFFLINE_FILE, "w");
  if (f) {
    f.print(rest);
    f.close();
  }
  if (offlineCount > 0) offlineCount--;
}

// Tambahkan satu payload ke akhir file buffer
static void queueOffline(const String& payload) {
  if (offlineCount >= MAX_OFFLINE_ENTRIES) dropOldestOffline();

  File f = LittleFS.open(OFFLINE_FILE, "a");
  if (!f) return;
  f.println(payload);
  f.close();
  offlineCount++;
  Serial.printf("[CLOUD] Disimpan ke buffer offline (%u/%u)\n", offlineCount, MAX_OFFLINE_ENTRIES);
}

// Gabungkan semua baris file buffer menjadi satu body: {"data":[payload,payload,...]}
// lineCount diisi jumlah payload yang digabung.
static String readOfflineBatch(File& f, int& lineCount) {
  String body = "{\"data\":[";
  lineCount = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    if (lineCount > 0) body += ',';
    body += line;
    lineCount++;
  }
  body += "]}";
  return body;
}

// Kirim seluruh isi buffer offline sekaligus. File dihapus jika terkirim.
static void flushOffline() {
  File f = LittleFS.open(OFFLINE_FILE, "r");
  if (!f) {
    offlineCount = 0;
    return;
  }
  int    lineCount = 0;
  String body      = readOfflineBatch(f, lineCount);
  f.close();

  if (lineCount == 0) {                // file ada tapi kosong
    LittleFS.remove(OFFLINE_FILE);
    offlineCount = 0;
    return;
  }

  int code = httpPost(CLOUD_BATCH_URL, body, nullptr);
  if (isSuccess(code)) {
    LittleFS.remove(OFFLINE_FILE);
    Serial.printf("[CLOUD] Buffer offline terkirim (%u data)\n", offlineCount);
    offlineCount = 0;
  } else {
    Serial.printf("[CLOUD] Kirim buffer offline gagal (HTTP %d), dicoba lagi nanti\n", code);
  }
}

// Simpan snapshot ke buffer offline, tetapi tidak lebih sering dari
// OFFLINE_SNAPSHOT_INTERVAL_MS supaya flash tidak cepat aus
static void saveSnapshotIfDue(uint32_t now) {
  if (now - lastSnapshotMs < OFFLINE_SNAPSHOT_INTERVAL_MS) return;
  lastSnapshotMs = now;
  queueOffline(buildTelemetry(false));
}

// =============================================================
//  Siklus kerja task
// =============================================================

static void onTelemetrySuccess(const String& response, uint32_t elapsedMs, bool wasQuickConfirm) {
  lastSendOk     = true;
  lastSendUptime = uptimeSec();
  otaConfirmBoot();                    // firmware ini terbukti bisa menghubungi server

  if (elapsedMs > CLOUD_SLOW_REQUEST_MS) {
    Serial.printf("[CLOUD] Telemetry lambat: %lu ms\n", (unsigned long)elapsedMs);
  }

  // Ada perintah dari web: laporkan kondisi baru secepatnya agar web cepat melihat
  // hasilnya. Hanya sekali berturut-turut, supaya server tidak dibanjiri request.
  quickConfirm = handleServerResponse(response) && !wasQuickConfirm;

  if (offlineCount > 0) flushOffline(); // server sudah terbukti bisa dihubungi
  otaService();                         // update firmware jika ditawarkan & semua zona idle
}

static void onTelemetryFailed(int code, uint32_t elapsedMs, uint32_t now) {
  lastSendOk = false;
  Serial.printf("[CLOUD] Kirim telemetry gagal (HTTP %d, %lu ms)\n", code, (unsigned long)elapsedMs);
  saveSnapshotIfDue(now);
}

static void sendTelemetry(uint32_t now) {
  bool wasQuickConfirm = quickConfirm;
  quickConfirm = false;

  String   payload = buildTelemetry(true);
  String   response;
  uint32_t startMs   = millis();
  int      code      = httpPost(CLOUD_URL, payload, &response);
  uint32_t elapsedMs = millis() - startMs;

  if (isSuccess(code)) onTelemetrySuccess(response, elapsedMs, wasQuickConfirm);
  else                 onTelemetryFailed(code, elapsedMs, now);
}

// WiFi STA terhubung: jalankan NTP dan kirim telemetry sesuai interval
static void handleOnline(uint32_t now) {
  clockServiceNtp();

  uint32_t interval = quickConfirm ? CLOUD_CONFIRM_DELAY_MS : CLOUD_SYNC_INTERVAL_MS;
  if (now - lastSendMs < interval) return;
  lastSendMs = now;
  sendTelemetry(now);
}

// Reconnect manual & berjeda: scan kanal saat reconnect mengganggu AP,
// jadi tidak dilakukan terus-menerus
static void reconnectWiFiIfDue(uint32_t now) {
  if (now - lastReconnectMs < WIFI_RECONNECT_INTERVAL_MS) return;
  lastReconnectMs = now;
  Serial.println("[WIFI] STA putus, mencoba reconnect...");
  WiFi.reconnect();
}

// WiFi STA putus: simpan data ke buffer offline dan coba reconnect
static void handleOffline(uint32_t now) {
  lastSendOk = false;
  saveSnapshotIfDue(now);
  reconnectWiFiIfDue(now);
}

void cloudSyncTask(void* param) {
  countOfflineEntries();
  Serial.printf("[CLOUD] %u data menunggu di buffer offline\n", offlineCount);
  lastReconnectMs = millis();

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED) handleOnline(now);
    else                               handleOffline(now);
  }
}
