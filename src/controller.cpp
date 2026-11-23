// =============================================================
//  CONTROLLER — aksi tingkat sistem
//  Ganti mode, antre irigasi, kontrol relay manual, dan simpan
//  pengaturan. Dipanggil dari Web API, CloudSyncTask, dan task lain.
// =============================================================
#include "controller.h"
#include "app_state.h"
#include "relay.h"
#include "datalog.h"
#include "settings_store.h"
#include "tasks.h"

// -------------------------------------------------------------
//  Helper
// -------------------------------------------------------------

// Bangunkan sebuah task (jika task-nya berhasil dibuat saat boot)
static void wakeTask(TaskHandle_t handle) {
  if (handle) xTaskNotifyGive(handle);
}

// Kembalikan semua zona ke kondisi Idle. Butuh lockState().
static void resetAllZones() {
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    app.zones[i].state       = ZoneState::Idle;
    app.zones[i].source      = TriggerSource::None;
    app.zones[i].durationSec = 0;
  }
}

bool isValidRelayId(int relay) {
  return relay == MAIN_RELAY_ID || (relay >= 0 && relay < NUM_ZONES);
}

// -------------------------------------------------------------
//  Mode sistem
// -------------------------------------------------------------

void setSystemMode(SystemMode mode) {
  lockState();
  if (app.mode == mode) {          // mode sama, tidak ada yang perlu diubah
    unlockState();
    return;
  }
  app.mode = mode;
  relayAllOff();                   // cegah relay tetap menyala saat pindah mode
  resetAllZones();
  unlockState();

  // IrrigationTask yang sedang berjalan akan melihat zona sudah Idle lalu berhenti
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    wakeTask(irrigationTaskHandles[i]);
  }

  // Masuk mode Auto -> evaluasi fuzzy langsung, tidak menunggu siklus sensor berikutnya
  if (mode == SystemMode::Auto) wakeTask(fuzzyTaskHandle);

  Serial.printf("[MODE] Mode sistem -> %s\n", toString(mode));
}

// -------------------------------------------------------------
//  Irigasi otomatis / terjadwal
// -------------------------------------------------------------

bool requestIrrigation(uint8_t zone, uint16_t durationSec, TriggerSource source) {
  if (zone >= NUM_ZONES || durationSec == 0) return false;

  lockState();
  Zone& z = app.zones[zone];
  bool accepted = (z.state == ZoneState::Idle && !z.relayOn);   // hanya jika zona sedang bebas
  if (accepted) {
    z.state       = ZoneState::Queued;
    z.durationSec = durationSec;
    z.source      = source;
  }
  unlockState();

  if (accepted) {
    wakeTask(irrigationTaskHandles[zone]);
    Serial.printf("[IRRIG] Zona %u antre: %u detik (%s)\n", zone + 1, durationSec, toString(source));
  }
  return accepted;
}

// -------------------------------------------------------------
//  Mode Manual
// -------------------------------------------------------------

// Buka/tutup valve satu zona. Butuh lockState().
// Jika valve ditutup, `entry` diisi data log dan fungsi return true.
static bool setManualZone(uint8_t zone, bool on, LogEntry& entry) {
  Zone& z     = app.zones[zone];
  bool  wasOn = z.relayOn;
  bool  shouldLog = false;

  // Valve baru dibuka -> simpan kelembaban awal
  if (on && !wasOn) z.soilBefore = z.soil;

  // Valve ditutup -> catat berapa lama valve terbuka
  if (!on && wasOn) {
    entry = makeLogEntry(app);
    entry.soilBefore[zone]  = z.soilBefore;
    entry.durationSec[zone] = (millis() - z.relayOnSince) / 1000;
    shouldLog = true;
  }

  relaySetZone(zone, on);
  relayFollowZones();              // pompa utama ON selama ada zona terbuka
  return shouldLog;
}

void manualSet(int relay, bool on) {
  bool     shouldLog = false;
  LogEntry entry;

  lockState();
  if (app.mode != SystemMode::Manual) {   // relay hanya boleh diatur pada mode Manual
    unlockState();
    return;
  }
  if (relay == MAIN_RELAY_ID) {
    relaySetMain(on);
  } else if (relay >= 0 && relay < NUM_ZONES) {
    shouldLog = setManualZone(relay, on, entry);
  }
  unlockState();

  // Tulis log setelah lock dilepas, karena menulis file itu lambat
  if (shouldLog) datalogAdd(entry);

  Serial.printf("[MANUAL] Relay %s -> %s\n",
                relay == MAIN_RELAY_ID ? "utama" : String(relay + 1).c_str(), on ? "ON" : "OFF");
}

bool manualToggle(int relay, String& error) {
  if (!isValidRelayId(relay)) {
    error = "relay tidak dikenal";
    return false;
  }

  lockState();
  bool isManual  = (app.mode == SystemMode::Manual);
  bool currentOn = (relay == MAIN_RELAY_ID) ? app.mainRelayOn : app.zones[relay].relayOn;
  unlockState();

  if (!isManual) {
    error = "relay hanya bisa dikontrol pada mode Manual";
    return false;
  }
  manualSet(relay, !currentOn);
  return true;
}

// -------------------------------------------------------------
//  Pengaturan & jadwal
// -------------------------------------------------------------

void applySettings(const Settings& s) {
  lockState();
  app.settings = s;
  unlockState();

  saveSettings(s);
  wakeTask(sensorTaskHandle);      // agar interval sensor yang baru langsung berlaku
}

void applySchedule(const ScheduleTable& t) {
  lockState();
  app.schedule = t;
  unlockState();

  saveSchedule(t);
}
