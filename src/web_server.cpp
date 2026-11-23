// =============================================================
//  WEB SERVER LOKAL (ESPAsyncWebServer)
//  REST API untuk dashboard + file dashboard yang tertanam di firmware.
//  Daftar endpoint ada di include/web_server.h.
// =============================================================
#include "web_server.h"
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "app_state.h"
#include "controller.h"
#include "settings_store.h"
#include "datalog.h"
#include "clock.h"
#include "tasks.h"
#include "www_assets.h"   // dibuat scripts/embed_www.py dari folder web/

static AsyncWebServer server(80);

// =============================================================
//  Helper respons
// =============================================================

static void sendJson(AsyncWebServerRequest* request, const JsonDocument& doc, int code = 200) {
  AsyncResponseStream* response = request->beginResponseStream("application/json");
  response->setCode(code);
  response->addHeader("Cache-Control", "no-store");
  serializeJson(doc, *response);
  request->send(response);
}

static void sendError(AsyncWebServerRequest* request, int code, const String& message) {
  JsonDocument doc;
  doc["ok"]    = false;
  doc["error"] = message;
  sendJson(request, doc, code);
}

static void sendOk(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(request, doc);
}

// Kirim file dashboard (sudah di-gzip). ETag + no-cache: browser selalu
// bertanya dulu dan cukup dibalas 304 jika file tidak berubah, sehingga
// dashboard baru langsung terpakai setelah update OTA.
static void sendAsset(AsyncWebServerRequest* request, const WebAsset& asset) {
  if (request->hasHeader("If-None-Match") && request->header("If-None-Match") == asset.etag) {
    return request->send(304);
  }
  AsyncWebServerResponse* response = request->beginResponse(200, asset.mime, asset.data, asset.len);
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-cache");
  response->addHeader("ETag", asset.etag);
  request->send(response);
}

// =============================================================
//  GET /api/data
// =============================================================

// Sisa detik dari sebuah hitung mundur (untuk countdown di dashboard)
static uint32_t remainingSec(uint32_t onSinceMs, uint32_t totalSec) {
  uint32_t elapsed = (millis() - onSinceMs) / 1000;
  return elapsed >= totalSec ? 0 : totalSec - elapsed;
}

// Sisa waktu siram satu zona: dari irigasi otomatis/terjadwal, atau dari timer manual
static uint32_t zoneRemainingSec(const Zone& z, uint16_t manualDurationSec, bool manualTimer) {
  if (z.state == ZoneState::Watering) return remainingSec(z.relayOnSince, z.durationSec);
  if (manualTimer && z.relayOn)       return remainingSec(z.relayOnSince, manualDurationSec);
  return 0;
}

// Sisa waktu cooldown satu zona (detik), 0 jika sudah boleh disiram lagi
static uint32_t zoneCooldownLeft(const Zone& z, uint32_t cooldownSec) {
  if (z.lastWateredUptime == 0) return 0;                // belum pernah disiram
  uint32_t since = uptimeSec() - z.lastWateredUptime;
  return since < cooldownSec ? cooldownSec - since : 0;
}

static void addSensorsJson(JsonObject out, const SensorReadings& sensors) {
  out["temperature"] = sensors.temperature;
  out["humidity"]    = sensors.humidity;
  out["lux"]         = sensors.lux;
  out["dhtOk"]       = sensors.dhtOk;
  out["luxOk"]       = sensors.luxOk;
}

static void addMainRelayJson(JsonObject out, const AppState& s, bool manualTimer) {
  out["on"]        = s.mainRelayOn;
  out["remaining"] = (manualTimer && s.mainRelayOn)
                         ? remainingSec(s.mainRelayOnSince, s.settings.manualDurationSec) : 0;
}

static void addZoneJson(JsonObject out, uint8_t index, const AppState& s, bool manualTimer) {
  const Zone& z = s.zones[index];
  out["id"]           = index + 1;
  out["soil"]         = z.soil;
  out["state"]        = toString(z.state);
  out["source"]       = toString(z.source);
  out["relay"]        = z.relayOn;
  out["duration"]     = z.durationSec;
  out["fuzzy"]        = z.fuzzyOutput;
  out["remaining"]    = zoneRemainingSec(z, s.settings.manualDurationSec, manualTimer);
  out["cooldownLeft"] = zoneCooldownLeft(z, s.settings.cooldownSec);
}

static void addTimeJson(JsonObject out) {
  out["valid"]  = clockIsValid();
  out["text"]   = clockFormat(clockNow());
  out["rtcOk"]  = clockRtcOk();
  out["source"] = toString(clockSource());
}

static void addWifiJson(JsonObject out) {
  bool staConnected = WiFi.status() == WL_CONNECTED;
  out["sta"]       = staConnected;
  out["rssi"]      = staConnected ? WiFi.RSSI() : 0;
  out["apClients"] = WiFi.softAPgetStationNum();
}

static void addCloudJson(JsonObject out) {
  CloudStatus cloud = cloudGetStatus();
  out["ok"]      = cloud.lastSendOk;
  out["lastAgo"] = cloud.lastSendUptime ? (int32_t)(uptimeSec() - cloud.lastSendUptime) : -1;
  out["queued"]  = cloud.offlineQueued;
}

static void handleData(AsyncWebServerRequest* request) {
  AppState s = copyState();
  bool manualTimer = s.mode == SystemMode::Manual && s.settings.manualType == ManualType::Timer;

  JsonDocument doc;
  doc["mode"]           = (uint8_t)s.mode;
  doc["manualType"]     = (uint8_t)s.settings.manualType;
  doc["manualDuration"] = s.settings.manualDurationSec;
  doc["uptime"]         = uptimeSec();
  doc["firmware"]       = FIRMWARE_VERSION;

  addSensorsJson(doc["sensors"].to<JsonObject>(), s.sensors);
  addMainRelayJson(doc["relayMain"].to<JsonObject>(), s, manualTimer);

  JsonArray zones = doc["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    addZoneJson(zones.add<JsonObject>(), i, s, manualTimer);
  }

  addTimeJson(doc["time"].to<JsonObject>());
  addWifiJson(doc["wifi"].to<JsonObject>());
  addCloudJson(doc["cloud"].to<JsonObject>());

  sendJson(request, doc);
}

// =============================================================
//  POST /api/override?mode=N  dan  POST /api/relay?relay=X
// =============================================================

static void handleOverride(AsyncWebServerRequest* request) {
  if (!request->hasParam("mode")) return sendError(request, 400, "parameter mode wajib");
  int mode = request->getParam("mode")->value().toInt();
  if (mode < 0 || mode > 2) return sendError(request, 400, "mode harus 0, 1, atau 2");

  setSystemMode((SystemMode)mode);
  sendOk(request);
}

static void handleRelay(AsyncWebServerRequest* request) {
  if (!request->hasParam("relay")) return sendError(request, 400, "parameter relay wajib");
  String value = request->getParam("relay")->value();
  int relay = (value == "main") ? MAIN_RELAY_ID : value.toInt() - 1;   // zona 1-based -> index

  String error;
  if (!manualToggle(relay, error)) return sendError(request, 409, error);
  sendOk(request);
}

// =============================================================
//  GET/POST /api/config
// =============================================================

static void handleGetConfig(AsyncWebServerRequest* request) {
  JsonDocument doc;
  settingsToJson(copyState().settings, doc.to<JsonObject>());
  sendJson(request, doc);
}

static void handlePostConfig(AsyncWebServerRequest* request, JsonVariant& json) {
  Settings s = copyState().settings;
  String error;
  if (!settingsFromJson(json, s, error)) return sendError(request, 400, error);

  applySettings(s);
  handleGetConfig(request);          // balas dengan konfigurasi terbaru
}

// =============================================================
//  GET/POST /api/schedule
// =============================================================

static void handleGetSchedule(AsyncWebServerRequest* request) {
  JsonDocument doc;
  scheduleToJson(copyState().schedule, doc.to<JsonArray>());
  sendJson(request, doc);
}

static void handlePostSchedule(AsyncWebServerRequest* request, JsonVariant& json) {
  ScheduleTable t = copyState().schedule;
  String error;
  if (!scheduleFromJson(json, t, error)) return sendError(request, 400, error);

  applySchedule(t);
  handleGetSchedule(request);        // balas dengan jadwal terbaru
}

// =============================================================
//  Log: GET /api/log, GET /api/log.csv, POST /api/log/reset
// =============================================================

static void logEntryToJson(const LogEntry& e, JsonObject out) {
  out["time"] = clockFormat(e.timestamp);
  out["temp"] = e.temperature;
  out["hum"]  = e.humidity;
  out["lux"]  = e.lux;
  out["mode"] = e.systemMode;

  JsonArray zones = out["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    JsonObject z  = zones.add<JsonObject>();
    z["before"]   = e.soilBefore[i];
    z["after"]    = e.soilAfter[i];
    z["duration"] = e.durationSec[i];
  }
}

static void handleGetLog(AsyncWebServerRequest* request) {
  size_t limit = MAX_LOG_ENTRIES;
  if (request->hasParam("limit")) {
    limit = constrain(request->getParam("limit")->value().toInt(), 1, MAX_LOG_ENTRIES);
  }

  JsonDocument doc;
  JsonArray rows = doc.to<JsonArray>();
  datalogForEach(limit, [&rows](const LogEntry& e) {
    logEntryToJson(e, rows.add<JsonObject>());
  });
  sendJson(request, doc);
}

// Label mode untuk kolom MODE di CSV
static const char* modeLabel(uint8_t mode) {
  if (mode == (uint8_t)SystemMode::Manual)   return "MANUAL";
  if (mode == (uint8_t)SystemMode::Schedule) return "PENJADWALAN";
  return "AUTO";
}

// Dua baris judul tabel CSV (baris kedua = sub-kolom tiap zona)
static void printCsvHeader(AsyncResponseStream* res) {
  res->print("\xEF\xBB\xBF");   // BOM agar Excel membaca UTF-8 (simbol °)

  res->print("WAKTU,SUHU (°C),KELEMBABAN UDARA (%),CAHAYA (Lux)");
  for (uint8_t i = 0; i < NUM_ZONES; i++) res->printf(",ZONA %u,,", i + 1);
  res->print(",MODE\r\n");

  res->print(",,,");
  for (uint8_t i = 0; i < NUM_ZONES; i++) res->print(",MOIS SEBELUM,MOIS SESUDAH,DURASI (dtk)");
  res->print(",AUTO / MANUAL / PENJADWALAN\r\n");
}

static void printCsvRow(AsyncResponseStream* res, const LogEntry& e) {
  res->printf("%s,%.1f,%.1f,%.0f", clockFormat(e.timestamp).c_str(), e.temperature, e.humidity, e.lux);
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    res->printf(",%.1f,%.1f,%u", e.soilBefore[i], e.soilAfter[i], e.durationSec[i]);
  }
  res->printf(",%s\r\n", modeLabel(e.systemMode));
}

static void handleLogCsv(AsyncWebServerRequest* request) {
  AsyncResponseStream* res = request->beginResponseStream("text/csv; charset=utf-8");
  res->addHeader("Content-Disposition", "attachment; filename=smart_nursery_log.csv");

  printCsvHeader(res);
  datalogForEach(MAX_LOG_ENTRIES, [res](const LogEntry& e) {
    printCsvRow(res, e);
  });
  request->send(res);
}

static void handleLogReset(AsyncWebServerRequest* request) {
  datalogClear();
  Serial.println("[LOG] Semua log dihapus dari dashboard");
  sendOk(request);
}

// =============================================================
//  POST /api/sync-time   body: {"epoch": 1728450000}
// =============================================================

static void handleSyncTime(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json["epoch"].is<long long>()) return sendError(request, 400, "epoch wajib diisi");
  time_t epoch = (time_t)json["epoch"].as<long long>();
  if (epoch < MIN_VALID_EPOCH) return sendError(request, 400, "epoch tidak valid");

  bool rtcWritten = clockSetFromBrowser(epoch);

  JsonDocument doc;
  doc["ok"]      = true;
  doc["rtc"]     = rtcWritten;
  doc["time"]    = clockFormat(clockNow());
  doc["message"] = rtcWritten
      ? "Waktu RTC berhasil disinkronkan"
      : "Jam sistem diperbarui, tetapi RTC DS3231 tidak merespons (cek wiring SDA/SCL). "
        "Waktu akan hilang saat perangkat restart.";
  sendJson(request, doc);
}

// =============================================================
//  Keamanan dashboard (berlaku untuk semua route, termasuk file dashboard)
//  1. Digest Auth: login dengan DASHBOARD_USER / DASHBOARD_PASSWORD (secrets.h).
//     Browser menyimpan kredensial setelah login, jadi fetch() di app.js ikut
//     terautentikasi tanpa kode tambahan.
//  2. Anti-CSRF: POST wajib membawa header X-Requested-With. Header kustom membuat
//     browser mengirim preflight CORS yang tidak dijawab server ini, sehingga
//     halaman web lain tidak bisa mengirim POST ke alat.
// =============================================================
static AsyncAuthenticationMiddleware authMiddleware;

static void setupLogin() {
  authMiddleware.setUsername(DASHBOARD_USER);
  authMiddleware.setPassword(DASHBOARD_PASSWORD);
  authMiddleware.setRealm("SmartNursery");
  authMiddleware.setAuthType(AsyncAuthType::AUTH_DIGEST);
  authMiddleware.setAuthFailureMessage("Login diperlukan");
  authMiddleware.generateHash();   // hash dihitung sekali saat boot, bukan per request
  server.addMiddleware(&authMiddleware);
}

// Dijalankan setelah login berhasil
static void setupCsrfProtection() {
  server.addMiddleware([](AsyncWebServerRequest* request, ArMiddlewareNext next) {
    if (request->method() == HTTP_POST && !request->hasHeader("X-Requested-With")) {
      return sendError(request, 403, "header X-Requested-With wajib");
    }
    next();
  });
}

// =============================================================
//  Registrasi route
// =============================================================

static void registerApiRoutes() {
  server.on("/api/data",     HTTP_GET,  handleData);
  server.on("/api/override", HTTP_POST, handleOverride);
  server.on("/api/relay",    HTTP_POST, handleRelay);

  server.on("/api/config",   HTTP_GET,  handleGetConfig);
  server.on("/api/config",   HTTP_POST, handlePostConfig);

  server.on("/api/schedule", HTTP_GET,  handleGetSchedule);
  server.on("/api/schedule", HTTP_POST, handlePostSchedule);

  server.on("/api/log.csv",   HTTP_GET,  handleLogCsv);
  server.on("/api/log/reset", HTTP_POST, handleLogReset);
  server.on("/api/log",       HTTP_GET,  handleGetLog);

  server.on("/api/sync-time", HTTP_POST, handleSyncTime);
}

// File dashboard dari flash program (web/ ditanam saat build), "/" -> index.html
static void registerDashboardRoutes() {
  for (const WebAsset& asset : WEB_ASSETS) {
    const WebAsset* a = &asset;   // elemen array statis, aman disimpan di handler
    server.on(a->path, HTTP_GET, [a](AsyncWebServerRequest* request) { sendAsset(request, *a); });
    if (strcmp(a->path, "/index.html") == 0) {
      server.on("/", HTTP_GET, [a](AsyncWebServerRequest* request) { sendAsset(request, *a); });
    }
  }
}

void webServerBegin() {
  setupLogin();
  setupCsrfProtection();

  registerApiRoutes();
  registerDashboardRoutes();

  server.onNotFound([](AsyncWebServerRequest* request) {
    sendError(request, 404, "tidak ditemukan: " + request->url());
  });

  server.begin();
  Serial.println("[WEB] Server berjalan di port 80");
}
