// =============================================================
//  IrrigationTask — satu task per zona.
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

void irrigationBegin() {
  irrigationSlots = xSemaphoreCreateCounting(MAX_CONCURRENT_IRRIGATION, MAX_CONCURRENT_IRRIGATION);
}

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
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(remaining));   // bangun jika ada notifikasi
    if (!zoneIs(zone, expected)) return false;
  }
  return true;
}

static void runIrrigation(uint8_t zone) {
  // ---- 1. Tunggu slot irigasi ----
  xSemaphoreTake(irrigationSlots, portMAX_DELAY);

  // ---- 2. Mulai menyiram ----
  lockState();
  Zone& z = app.zones[zone];
  if (z.state != ZoneState::Queued) {        // dibatalkan saat masih antre
    unlockState();
    xSemaphoreGive(irrigationSlots);
    return;
  }
  uint16_t      durationSec = z.durationSec;
  TriggerSource source      = z.source;
  z.state      = ZoneState::Watering;
  z.soilBefore = z.soil;
  float soilBefore = z.soil;
  relaySetZone(zone, true);
  relayFollowZones();                        // pompa utama ikut menyala
  unlockState();

  uint32_t startMs = millis();
  Serial.printf("[IRRIG] Zona %u MULAI siram %u detik (%s)\n", zone + 1, durationSec, toString(source));

  // ---- 3. Tunggu durasi siram ----
  bool completed = waitWhileState(zone, ZoneState::Watering, (uint32_t)durationSec * 1000);
  uint16_t wateredSec = (millis() - startMs) / 1000;

  // ---- 4. Matikan relay ----
  lockState();
  if (app.zones[zone].state == ZoneState::Watering) {
    // Masih milik task ini. Jika dibatalkan, controller sudah mematikan relay.
    relaySetZone(zone, false);
    relayFollowZones();
    app.zones[zone].state = ZoneState::Soaking;
  }
  unlockState();
  xSemaphoreGive(irrigationSlots);

  // ---- 5. Jeda serap air sebelum membaca "soil sesudah" ----
  if (completed) {
    Serial.printf("[IRRIG] Zona %u selesai, jeda serap %u detik\n", zone + 1, SOAK_DELAY_SEC);
    completed = waitWhileState(zone, ZoneState::Soaking, (uint32_t)SOAK_DELAY_SEC * 1000);
  }

  // ---- 6. Catat log & kembali Idle ----
  lockState();
  Zone& done = app.zones[zone];
  if (done.state == ZoneState::Soaking) done.state = ZoneState::Idle;
  done.lastWateredUptime = uptimeSec();
  done.durationSec       = 0;
  done.source            = TriggerSource::None;

  LogEntry entry           = makeLogEntry(app);
  entry.soilBefore[zone]   = soilBefore;
  entry.durationSec[zone]  = wateredSec;
  unlockState();

  datalogAdd(entry);
  Serial.printf("[IRRIG] Zona %u %s (disiram %u detik)\n",
                zone + 1, completed ? "SELESAI" : "DIBATALKAN", wateredSec);
}

void irrigationTask(void* param) {
  const uint8_t zone = (uint8_t)(uintptr_t)param;

  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);           // tunggu permintaan
    if (zoneIs(zone, ZoneState::Queued)) runIrrigation(zone);
  }
}
