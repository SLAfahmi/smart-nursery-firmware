// =============================================================
//  CloudSyncTask (Core 0) — sinkronisasi dengan server Laravel
//
//  WiFi STA terhubung:
//    - kirim telemetry ke CLOUD_URL setiap CLOUD_SYNC_INTERVAL_MS
//    - jika sukses: terapkan `device_command` dari server, lalu kirim
//      isi buffer offline (batch) ke CLOUD_BATCH_URL, lalu jalankan
//      OTA jika server menawarkan firmware baru (src/ota.cpp)
//    - jika gagal : simpan payload ke buffer offline
//    - jalankan NTP & salin waktunya ke RTC
//  WiFi STA putus:
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

static volatile bool     lastSendOk     = false;
static volatile uint32_t lastSendUptime = 0;
static volatile uint16_t offlineCount   = 0;

// Versi device_command terakhir yang diterapkan (0 setelah boot). Server mengirim
// konfigurasi penuh selama versi ini berbeda dengan versi di web.
static uint32_t appliedCommandVersion = 0;

CloudStatus cloudGetStatus() {
  return {lastSendOk, lastSendUptime, offlineCount};
}

// -------------------------------------------------------------
//  Payload telemetry
//  includeConfig: sertakan versi perintah + konfigurasi & jadwal yang sedang
//  berlaku, agar perubahan dari dashboard lokal ikut tampil di web, serta
//  versi firmware & status OTA.
//  Tidak dipakai untuk buffer offline (data lama, dan memperbesar ukuran batch).
// -------------------------------------------------------------
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

  JsonArray zones = doc["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    JsonObject z = zones.add<JsonObject>();
    z["zone"]           = i + 1;
    z["soil"]           = s.zones[i].soil;
    z["relay"]          = s.zones[i].relayOn;
    z["state"]          = toString(s.zones[i].state);
    z["fuzzy_duration"] = s.zones[i].fuzzyOutput;
  }

  if (includeConfig) {
    doc["command_version"]  = appliedCommandVersion;
    doc["firmware_version"] = FIRMWARE_VERSION;
    settingsToJson(s.settings, doc["config"].to<JsonObject>());
    scheduleToJson(s.schedule, doc["schedules"].to<JsonArray>());
    otaReport(doc.as<JsonObject>());
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// -------------------------------------------------------------
//  HTTP
// -------------------------------------------------------------
// Return kode HTTP (>0) atau kode error HTTPClient (<0)
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

// -------------------------------------------------------------
//  Perintah dari server (device_command)
//  { "device_command": { "version": n, "system_mode": 0..2,
//                        "manual_mode_type": 0..1, "manual_duration": detik,
//                        "sensor_interval": detik, "log_interval": detik,
//                        "cooldown": detik, "schedules": [...],
//                        "relays": [{"relay": "main"|1..N, "on": bool}] } }
//  Semua key opsional. Format "schedules" sama dengan GET /api/schedule.
// -------------------------------------------------------------

// Relay hanya dijalankan pada mode Manual (dicek di manualSet)
static void applyRelayCommands(JsonArrayConst relays) {
  for (JsonVariantConst r : relays) {
    JsonVariantConst id = r["relay"];
    int relay;
    if (id.is<const char*>() && strcmp(id.as<const char*>(), "main") == 0) relay = MAIN_RELAY_ID;
    else if (id.is<int>())                                                 relay = id.as<int>() - 1;   // zona 1-based -> index
    else continue;

    if (relay != MAIN_RELAY_ID && (relay < 0 || relay >= NUM_ZONES)) continue;
    if (!r["on"].is<bool>()) continue;

    if (copyState().mode != SystemMode::Manual) {
      Serial.println("[CLOUD] Perintah relay diabaikan: bukan mode Manual");
      return;
    }
    manualSet(relay, r["on"].as<bool>());
  }
}

static void applyDeviceCommand(JsonVariantConst cmd) {
  if (cmd.isNull()) return;
  String error;

  // 1. Mode sistem
  if (cmd["system_mode"].is<int>()) {
    int mode = cmd["system_mode"];
    if (mode >= 0 && mode <= 2) setSystemMode((SystemMode)mode);
  }

  // 2. Pengaturan (nama key cloud -> nama key lokal /api/config)
  JsonDocument local;
  if (!cmd["manual_mode_type"].isNull()) local["manualMode"]     = cmd["manual_mode_type"];
  if (!cmd["manual_duration"].isNull())  local["manualDuration"] = cmd["manual_duration"];
  if (!cmd["sensor_interval"].isNull())  local["sensorInterval"] = cmd["sensor_interval"];
  if (!cmd["log_interval"].isNull())     local["logInterval"]    = cmd["log_interval"];
  if (!cmd["cooldown"].isNull())         local["cooldown"]       = cmd["cooldown"];
  if (local.size() > 0) {
    Settings current = copyState().settings;
    Settings updated = current;
    if (!settingsFromJson(local.as<JsonVariantConst>(), updated, error)) {
      Serial.printf("[CLOUD] device_command pengaturan ditolak: %s\n", error.c_str());
    } else if (updated != current) {
      applySettings(updated);
      Serial.println("[CLOUD] Pengaturan diperbarui dari server");
    }
  }

  // 3. Jadwal
  if (!cmd["schedules"].isNull()) {
    ScheduleTable current = copyState().schedule;
    ScheduleTable updated = current;
    if (!scheduleFromJson(cmd["schedules"], updated, error)) {
      Serial.printf("[CLOUD] device_command jadwal ditolak: %s\n", error.c_str());
    } else if (updated != current) {
      applySchedule(updated);
      Serial.println("[CLOUD] Jadwal diperbarui dari server");
    }
  }

  // 4. Relay (setelah mode, agar "ganti ke Manual + nyalakan relay" bisa dalam satu perintah)
  if (cmd["relays"].is<JsonArrayConst>()) applyRelayCommands(cmd["relays"].as<JsonArrayConst>());

  // 5. Catat versi yang sudah diterapkan
  if (cmd["version"].is<uint32_t>()) {
    uint32_t version = cmd["version"];
    if (version != appliedCommandVersion) {
      appliedCommandVersion = version;
      Serial.printf("[CLOUD] Konfigurasi versi %lu diterapkan\n", (unsigned long)version);
    }
  }
}

// Return true jika server mengirim device_command
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

// -------------------------------------------------------------
//  Buffer offline (LittleFS)
// -------------------------------------------------------------
static void countOfflineEntries() {
  offlineCount = 0;
  File f = LittleFS.open(OFFLINE_FILE, "r");
  if (!f) return;
  while (f.available()) {
    if (f.readStringUntil('\n').length() > 1) offlineCount++;
  }
  f.close();
}

static void dropOldestOffline() {
  File f = LittleFS.open(OFFLINE_FILE, "r");
  if (!f) return;
  f.readStringUntil('\n');            // buang baris pertama (terlama)
  String rest = f.readString();
  f.close();

  f = LittleFS.open(OFFLINE_FILE, "w");
  if (f) {
    f.print(rest);
    f.close();
  }
  if (offlineCount > 0) offlineCount--;
}

static void queueOffline(const String& payload) {
  if (offlineCount >= MAX_OFFLINE_ENTRIES) dropOldestOffline();

  File f = LittleFS.open(OFFLINE_FILE, "a");
  if (!f) return;
  f.println(payload);
  f.close();
  offlineCount++;
  Serial.printf("[CLOUD] Disimpan ke buffer offline (%u/%u)\n", offlineCount, MAX_OFFLINE_ENTRIES);
}

// Kirim semua isi buffer sebagai {"data":[payload, payload, ...]}
static void flushOffline() {
  File f = LittleFS.open(OFFLINE_FILE, "r");
  if (!f) {
    offlineCount = 0;
    return;
  }

  String body = "{\"data\":[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    if (!first) body += ',';
    body += line;
    first = false;
  }
  f.close();
  body += "]}";

  if (first) {                        // file ada tapi kosong
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

// -------------------------------------------------------------
//  Task
// -------------------------------------------------------------
void cloudSyncTask(void* param) {
  countOfflineEntries();
  Serial.printf("[CLOUD] %u data menunggu di buffer offline\n", offlineCount);

  uint32_t lastSendMs      = 0;
  uint32_t lastSnapshotMs  = 0;
  uint32_t lastReconnectMs = millis();
  bool     quickConfirm    = false;   // kirim berikutnya lebih cepat (setelah ada perintah web)

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED) {
      clockServiceNtp();

      if (now - lastSendMs < (quickConfirm ? CLOUD_CONFIRM_DELAY_MS : CLOUD_SYNC_INTERVAL_MS)) continue;
      lastSendMs = now;
      bool wasQuickConfirm = quickConfirm;
      quickConfirm = false;

      String payload = buildTelemetry(true);
      String response;
      uint32_t startMs = millis();
      int code = httpPost(CLOUD_URL, payload, &response);
      uint32_t elapsedMs = millis() - startMs;

      if (isSuccess(code)) {
        lastSendOk     = true;
        lastSendUptime = uptimeSec();
        otaConfirmBoot();                       // firmware ini terbukti bisa menghubungi server
        if (elapsedMs > CLOUD_SLOW_REQUEST_MS) {
          Serial.printf("[CLOUD] Telemetry lambat: %lu ms\n", (unsigned long)elapsedMs);
        }
        // Ada perintah dari web: laporkan kondisi baru secepatnya agar web cepat melihat
        // hasilnya. Hanya sekali berturut-turut, supaya server tidak dibanjiri request.
        quickConfirm = handleServerResponse(response) && !wasQuickConfirm;
        if (offlineCount > 0) flushOffline();   // server sudah terbukti bisa dihubungi
        otaService();                           // update firmware jika ditawarkan & semua zona idle
      } else {
        lastSendOk = false;
        Serial.printf("[CLOUD] Kirim telemetry gagal (HTTP %d, %lu ms)\n", code, (unsigned long)elapsedMs);
        // Server error: simpan secukupnya, jangan setiap 5 detik agar flash awet
        if (now - lastSnapshotMs >= OFFLINE_SNAPSHOT_INTERVAL_MS) {
          lastSnapshotMs = now;
          queueOffline(buildTelemetry(false));
        }
      }
    } else {
      lastSendOk = false;

      if (now - lastSnapshotMs >= OFFLINE_SNAPSHOT_INTERVAL_MS) {
        lastSnapshotMs = now;
        queueOffline(buildTelemetry(false));
      }

      // Reconnect manual & berjeda: scan kanal saat reconnect mengganggu AP,
      // jadi tidak dilakukan terus-menerus.
      if (now - lastReconnectMs >= WIFI_RECONNECT_INTERVAL_MS) {
        lastReconnectMs = now;
        Serial.println("[WIFI] STA putus, mencoba reconnect...");
        WiFi.reconnect();
      }
    }
  }
}
