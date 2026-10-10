#include "web_server.h"
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "app_state.h"
#include "controller.h"
#include "settings_store.h"
#include "datalog.h"
#include "clock.h"
#include "tasks.h"

static AsyncWebServer server(80);

// -------------------------------------------------------------
//  Helper respons
// -------------------------------------------------------------
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

// Sisa detik relay menyala (untuk countdown di dashboard)
static uint32_t remainingSec(uint32_t onSinceMs, uint32_t totalSec) {
  uint32_t elapsed = (millis() - onSinceMs) / 1000;
  return elapsed >= totalSec ? 0 : totalSec - elapsed;
}

// -------------------------------------------------------------
//  GET /api/data
// -------------------------------------------------------------
static void handleData(AsyncWebServerRequest* request) {
  AppState s = copyState();
  bool manualTimer = s.mode == SystemMode::Manual && s.settings.manualType == ManualType::Timer;

  JsonDocument doc;
  doc["mode"]           = (uint8_t)s.mode;
  doc["manualType"]     = (uint8_t)s.settings.manualType;
  doc["manualDuration"] = s.settings.manualDurationSec;
  doc["uptime"]         = uptimeSec();

  JsonObject sensors = doc["sensors"].to<JsonObject>();
  sensors["temperature"] = s.sensors.temperature;
  sensors["humidity"]    = s.sensors.humidity;
  sensors["lux"]         = s.sensors.lux;
  sensors["dhtOk"]       = s.sensors.dhtOk;
  sensors["luxOk"]       = s.sensors.luxOk;

  JsonObject main = doc["relayMain"].to<JsonObject>();
  main["on"]        = s.mainRelayOn;
  main["remaining"] = (manualTimer && s.mainRelayOn)
                          ? remainingSec(s.mainRelayOnSince, s.settings.manualDurationSec) : 0;

  JsonArray zones = doc["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    const Zone& z = s.zones[i];
    JsonObject o = zones.add<JsonObject>();
    o["id"]       = i + 1;
    o["soil"]     = z.soil;
    o["state"]    = toString(z.state);
    o["source"]   = toString(z.source);
    o["relay"]    = z.relayOn;
    o["duration"] = z.durationSec;
    o["fuzzy"]    = z.fuzzyOutput;

    uint32_t remaining = 0;
    if (z.state == ZoneState::Watering) remaining = remainingSec(z.relayOnSince, z.durationSec);
    else if (manualTimer && z.relayOn)  remaining = remainingSec(z.relayOnSince, s.settings.manualDurationSec);
    o["remaining"] = remaining;

    uint32_t cooldownLeft = 0;
    if (z.lastWateredUptime != 0) {
      uint32_t since = uptimeSec() - z.lastWateredUptime;
      if (since < s.settings.cooldownSec) cooldownLeft = s.settings.cooldownSec - since;
    }
    o["cooldownLeft"] = cooldownLeft;
  }

  JsonObject time = doc["time"].to<JsonObject>();
  time["valid"]  = clockIsValid();
  time["text"]   = clockFormat(clockNow());
  time["rtcOk"]  = clockRtcOk();
  time["source"] = toString(clockSource());

  bool staConnected = WiFi.status() == WL_CONNECTED;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["sta"]       = staConnected;
  wifi["rssi"]      = staConnected ? WiFi.RSSI() : 0;
  wifi["apClients"] = WiFi.softAPgetStationNum();

  CloudStatus cloud = cloudGetStatus();
  JsonObject c = doc["cloud"].to<JsonObject>();
  c["ok"]      = cloud.lastSendOk;
  c["lastAgo"] = cloud.lastSendUptime ? (int32_t)(uptimeSec() - cloud.lastSendUptime) : -1;
  c["queued"]  = cloud.offlineQueued;

  sendJson(request, doc);
}

// -------------------------------------------------------------
//  POST /api/override?mode=N  dan  POST /api/relay?relay=X
// -------------------------------------------------------------
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

// -------------------------------------------------------------
//  /api/config
// -------------------------------------------------------------
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
  handleGetConfig(request);
}

// -------------------------------------------------------------
//  /api/schedule
// -------------------------------------------------------------
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
  handleGetSchedule(request);
}

// -------------------------------------------------------------
//  Log
// -------------------------------------------------------------
static void handleGetLog(AsyncWebServerRequest* request) {
  size_t limit = MAX_LOG_ENTRIES;
  if (request->hasParam("limit")) limit = constrain(request->getParam("limit")->value().toInt(), 1, MAX_LOG_ENTRIES);

  JsonDocument doc;
  JsonArray rows = doc.to<JsonArray>();
  datalogForEach(limit, [&rows](const LogEntry& e) {
    JsonObject r = rows.add<JsonObject>();
    r["time"]     = clockFormat(e.timestamp);
    r["temp"]     = e.temperature;
    r["hum"]      = e.humidity;
    r["lux"]      = e.lux;
    r["mode"]     = e.systemMode;
    JsonArray zones = r["zones"].to<JsonArray>();
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
      JsonObject z = zones.add<JsonObject>();
      z["before"]   = e.soilBefore[i];
      z["after"]    = e.soilAfter[i];
      z["duration"] = e.durationSec[i];
    }
  });
  sendJson(request, doc);
}

static const char* modeLabel(uint8_t mode) {
  if (mode == (uint8_t)SystemMode::Manual)   return "MANUAL";
  if (mode == (uint8_t)SystemMode::Schedule) return "PENJADWALAN";
  return "AUTO";
}

static void handleLogCsv(AsyncWebServerRequest* request) {
  AsyncResponseStream* res = request->beginResponseStream("text/csv; charset=utf-8");
  res->addHeader("Content-Disposition", "attachment; filename=smart_nursery_log.csv");

  res->print("\xEF\xBB\xBF");   // BOM agar Excel membaca UTF-8 (simbol °)
  res->print("WAKTU,SUHU (°C),KELEMBABAN UDARA (%),CAHAYA (Lux)");
  for (uint8_t i = 0; i < NUM_ZONES; i++) res->printf(",ZONA %u,,", i + 1);
  res->print(",MODE\r\n,,,");
  for (uint8_t i = 0; i < NUM_ZONES; i++) res->print(",MOIS SEBELUM,MOIS SESUDAH,DURASI (dtk)");
  res->print(",AUTO / MANUAL / PENJADWALAN\r\n");

  datalogForEach(MAX_LOG_ENTRIES, [res](const LogEntry& e) {
    res->printf("%s,%.1f,%.1f,%.0f", clockFormat(e.timestamp).c_str(), e.temperature, e.humidity, e.lux);
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
      res->printf(",%.1f,%.1f,%u", e.soilBefore[i], e.soilAfter[i], e.durationSec[i]);
    }
    res->printf(",%s\r\n", modeLabel(e.systemMode));
  });
  request->send(res);
}

static void handleLogReset(AsyncWebServerRequest* request) {
  datalogClear();
  Serial.println("[LOG] Semua log dihapus dari dashboard");
  sendOk(request);
}

// -------------------------------------------------------------
//  POST /api/sync-time   body: {"epoch": 1728450000}
// -------------------------------------------------------------
static void handleSyncTime(AsyncWebServerRequest* request, JsonVariant& json) {
  if (!json["epoch"].is<long long>()) return sendError(request, 400, "epoch wajib diisi");
  time_t epoch = (time_t)json["epoch"].as<long long>();
  if (epoch < 1704067200) return sendError(request, 400, "epoch tidak valid");

  bool rtcWritten = clockSetFromBrowser(epoch);

  JsonDocument doc;
  doc["ok"]   = true;
  doc["rtc"]  = rtcWritten;
  doc["time"] = clockFormat(clockNow());
  doc["message"] = rtcWritten
      ? "Waktu RTC berhasil disinkronkan"
      : "Jam sistem diperbarui, tetapi RTC DS3231 tidak merespons (cek wiring SDA/SCL). "
        "Waktu akan hilang saat perangkat restart.";
  sendJson(request, doc);
}

// -------------------------------------------------------------
//  Registrasi route
// -------------------------------------------------------------
void webServerBegin() {
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/override", HTTP_POST, handleOverride);
  server.on("/api/relay", HTTP_POST, handleRelay);

  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);

  server.on("/api/schedule", HTTP_GET, handleGetSchedule);
  server.on("/api/schedule", HTTP_POST, handlePostSchedule);

  server.on("/api/log.csv", HTTP_GET, handleLogCsv);
  server.on("/api/log/reset", HTTP_POST, handleLogReset);
  server.on("/api/log", HTTP_GET, handleGetLog);

  server.on("/api/sync-time", HTTP_POST, handleSyncTime);

  // File dashboard. Di-cache browser 10 menit supaya tidak diunduh ulang tiap buka.
  server.serveStatic("/", LittleFS, "/www/")
      .setDefaultFile("index.html")
      .setCacheControl("max-age=600");

  server.onNotFound([](AsyncWebServerRequest* request) {
    sendError(request, 404, "tidak ditemukan: " + request->url());
  });

  server.begin();
  Serial.println("[WEB] Server berjalan di port 80");
}
