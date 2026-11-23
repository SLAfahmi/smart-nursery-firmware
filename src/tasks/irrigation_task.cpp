// =============================================================
//  IrrigationTask — satu task untuk setiap zona.
//
//  Alur: Queued -> (tunggu slot) -> Watering: relay ON selama durasi
//        -> relay OFF -> Soaking: jeda serap air -> catat log -> Idle
//
//  State zona di `app` adalah sumber kebenaran. Notifikasi task hanya
//  dipakai untuk "membangunkan". Jika mode diganti, controller mengubah
//  state zona ke Idle, dan task ini berhenti begitu melihatnya.
// =============================================================
#include "tasks.h"
#include "app_state.h"
#include "relay.h"
#include "datalog.h"

// Membatasi jumlah zona yang menyiram bersamaan (kapasitas pompa)
static SemaphoreHandle_t irrigationSlots = nullptr;

// Data penyiraman yang dicatat saat mulai, dipakai lagi saat menulis log
struct WateringInfo {
  uint16_t      durationSec;
  TriggerSource source;
  float         soilBefore;
};

void irrigationBegin() {
  irrigationSlots = xSemaphoreCreateCounting(MAX_CONCURRENT_IRRIGATION, MAX_CONCURRENT_IRRIGATION);
}

// -------------------------------------------------------------
//  Helper
// -------------------------------------------------------------

static bool zoneIs(uint8_t zone, ZoneState expected) {
  lockState();
  bool same = (app.zones[zone].state == expected);
  unlockState();
  return same;
}

// Tunggu `ms` milidetik selama state zona masih `expected`.
// Return true jika waktu habis normal, false jika dibatalkan di tengah jalan.
static bool waitWhileState(uint8_t zone, ZoneState expected, uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    uint32_t remaining = ms - (millis() - start);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(remaining));   // bangun lebih awal jika ada notifikasi
    if (!zoneIs(zone, expected)) return false;
  }
  return true;
}

// -------------------------------------------------------------
//  Tahapan irigasi
// -------------------------------------------------------------

// Ubah zona dari Queued ke Watering, lalu nyalakan valve + pompa.
// Return false jika permintaan sudah dibatalkan saat masih antre.
static bool startWatering(uint8_t zone, WateringInfo& info) {
  lockState();
  Zone& z = app.zones[zone];
  if (z.state != ZoneState::Queued) {
    unlockState();
    return false;
  }

  info.durationSec = z.durationSec;
  info.source      = z.source;
  info.soilBefore  = z.soil;

  z.state      = ZoneState::Watering;
  z.soilBefore = z.soil;
  relaySetZone(zone, true);
  relayFollowZones();                        // pompa utama ikut menyala
  unlockState();
  return true;
}

// Matikan valve dan masuk tahap Soaking.
// Jika irigasi sudah dibatalkan, controller sudah mematikan relay, jadi dilewati.
static void stopWatering(uint8_t zone) {
  lockState();
  if (app.zones[zone].state == ZoneState::Watering) {
    relaySetZone(zone, false);
    relayFollowZones();
    app.zones[zone].state = ZoneState::Soaking;
  }
  unlockState();
}

// Kembalikan zona ke Idle dan siapkan data log penyiraman
static LogEntry finishIrrigation(uint8_t zone, float soilBefore, uint16_t wateredSec) {
  lockState();
  Zone& z = app.zones[zone];
  if (z.state == ZoneState::Soaking) z.state = ZoneState::Idle;
  z.lastWateredUptime = uptimeSec();
  z.durationSec       = 0;
  z.source            = TriggerSource::None;

  LogEntry entry          = makeLogEntry(app);   // "soil sesudah" = kondisi sekarang
  entry.soilBefore[zone]  = soilBefore;
  entry.durationSec[zone] = wateredSec;
  unlockState();
  return entry;
}

static void runIrrigation(uint8_t zone) {
  // 1. Tunggu slot irigasi
  xSemaphoreTake(irrigationSlots, portMAX_DELAY);

  // 2. Mulai menyiram
  WateringInfo info;
  if (!startWatering(zone, info)) {          // dibatalkan saat masih antre
    xSemaphoreGive(irrigationSlots);
    return;
  }
  uint32_t startMs = millis();
  Serial.printf("[IRRIG] Zona %u MULAI siram %u detik (%s)\n", zone + 1, info.durationSec, toString(info.source));

  // 3. Tunggu sampai durasi siram habis (atau dibatalkan)
  bool     completed  = waitWhileState(zone, ZoneState::Watering, (uint32_t)info.durationSec * 1000);
  uint16_t wateredSec = (millis() - startMs) / 1000;

  // 4. Matikan relay & lepas slot
  stopWatering(zone);
  xSemaphoreGive(irrigationSlots);

  // 5. Jeda serap air sebelum membaca "soil sesudah"
  if (completed) {
    Serial.printf("[IRRIG] Zona %u selesai, jeda serap %u detik\n", zone + 1, SOAK_DELAY_SEC);
    completed = waitWhileState(zone, ZoneState::Soaking, (uint32_t)SOAK_DELAY_SEC * 1000);
  }

  // 6. Catat log & kembali Idle
  LogEntry entry = finishIrrigation(zone, info.soilBefore, wateredSec);
  datalogAdd(entry);
  Serial.printf("[IRRIG] Zona %u %s (disiram %u detik)\n",
                zone + 1, completed ? "SELESAI" : "DIBATALKAN", wateredSec);
}

// -------------------------------------------------------------
//  Task
// -------------------------------------------------------------

void irrigationTask(void* param) {
  const uint8_t zone = (uint8_t)(uintptr_t)param;

  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);           // tunggu permintaan dari controller
    if (zoneIs(zone, ZoneState::Queued)) runIrrigation(zone);
  }
}
