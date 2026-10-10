// =============================================================
//  CloudSyncTask (Core 0) — sinkronisasi dengan server Laravel
//
//  WiFi STA terhubung:
//    - kirim telemetry ke CLOUD_URL setiap CLOUD_SYNC_INTERVAL_MS
//    - jika sukses: terapkan `device_command` dari server, lalu kirim
//      isi buffer offline (batch) ke CLOUD_BATCH_URL
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

static const char* OFFLINE_FILE = "/offline_q.jsonl";

static volatile bool     lastSendOk     = false;
static volatile uint32_t lastSendUptime = 0;
static volatile uint16_t offlineCount   = 0;

CloudStatus cloudGetStatus() {
  return {lastSendOk, lastSendUptime, offlineCount};
}

// -------------------------------------------------------------
//  Payload telemetry
// -------------------------------------------------------------
static String buildTelemetry() {
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
//  { "device_command": { "system_mode": 0..2, "manual_mode_type": 0..1,
//                        "manual_duration": detik, "schedules": [...] } }
//  Format "schedules" sama dengan GET /api/schedule.
// -------------------------------------------------------------
static void applyDeviceCommand(JsonVariantConst cmd) {
  if (cmd.isNull()) return;
  String error;

  // 1. Mode sistem
  if (cmd["system_mode"].is<int>()) {
    int mode = cmd["system_mode"];
    if (mode >= 0 && mode <= 2) setSystemMode((SystemMode)mode);
  }

  // 2. Pengaturan manual (nama key cloud -> nama key lokal)
  JsonDocument local;
  if (!cmd["manual_mode_type"].isNull()) local["manualMode"]     = cmd["manual_mode_type"];
  if (!cmd["manual_duration"].isNull())  local["manualDuration"] = cmd["manual_duration"];
  if (local.size() > 0) {
    Settings current = copyState().settings;
    Settings updated = current;
    if (!settingsFromJson(local.as<JsonVariantConst>(), updated, error)) {
      Serial.printf("[CLOUD] device_command manual ditolak: %s\n", error.c_str());
    } else if (updated != current) {
      applySettings(updated);
      Serial.println("[CLOUD] Pengaturan manual diperbarui dari server");
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
}

static void handleServerResponse(const String& response) {
  if (response.isEmpty()) return;
  JsonDocument doc;
  if (deserializeJson(doc, response)) return;   // bukan JSON, abaikan

  JsonVariantConst cmd = doc["device_command"];
  if (cmd.isNull()) cmd = doc["data"]["device_command"];
  applyDeviceCommand(cmd);
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

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED) {
      clockServiceNtp();

      if (now - lastSendMs < CLOUD_SYNC_INTERVAL_MS) continue;
      lastSendMs = now;

      String payload = buildTelemetry();
      String response;
      int code = httpPost(CLOUD_URL, payload, &response);

      if (isSuccess(code)) {
        lastSendOk     = true;
        lastSendUptime = uptimeSec();
        handleServerResponse(response);
        if (offlineCount > 0) flushOffline();   // server sudah terbukti bisa dihubungi
      } else {
        lastSendOk = false;
        Serial.printf("[CLOUD] Kirim telemetry gagal (HTTP %d)\n", code);
        // Server error: simpan secukupnya, jangan setiap 5 detik agar flash awet
        if (now - lastSnapshotMs >= OFFLINE_SNAPSHOT_INTERVAL_MS) {
          lastSnapshotMs = now;
          queueOffline(payload);
        }
      }
    } else {
      lastSendOk = false;

      if (now - lastSnapshotMs >= OFFLINE_SNAPSHOT_INTERVAL_MS) {
        lastSnapshotMs = now;
        queueOffline(buildTelemetry());
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
