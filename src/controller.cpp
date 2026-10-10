#include "controller.h"
#include "app_state.h"
#include "relay.h"
#include "datalog.h"
#include "settings_store.h"
#include "tasks.h"

void setSystemMode(SystemMode mode) {
  lockState();
  if (app.mode == mode) {
    unlockState();
    return;
  }
  app.mode = mode;

  // Cegah state menggantung: matikan semua relay & reset zona
  relayAllOff();
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    app.zones[i].state       = ZoneState::Idle;
    app.zones[i].source      = TriggerSource::None;
    app.zones[i].durationSec = 0;
  }
  unlockState();

  // Bangunkan IrrigationTask; task melihat state zona sudah Idle lalu berhenti
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    if (irrigationTaskHandles[i]) xTaskNotifyGive(irrigationTaskHandles[i]);
  }

  // Masuk mode Auto -> evaluasi fuzzy langsung, tidak menunggu siklus sensor berikutnya
  if (mode == SystemMode::Auto && fuzzyTaskHandle) xTaskNotifyGive(fuzzyTaskHandle);

  Serial.printf("[MODE] Mode sistem -> %s\n", toString(mode));
}

bool requestIrrigation(uint8_t zone, uint16_t durationSec, TriggerSource source) {
  if (zone >= NUM_ZONES || durationSec == 0) return false;

  lockState();
  Zone& z = app.zones[zone];
  bool accepted = (z.state == ZoneState::Idle && !z.relayOn);
  if (accepted) {
    z.state       = ZoneState::Queued;
    z.durationSec = durationSec;
    z.source      = source;
  }
  unlockState();

  if (accepted) {
    xTaskNotifyGive(irrigationTaskHandles[zone]);
    Serial.printf("[IRRIG] Zona %u antre: %u detik (%s)\n", zone + 1, durationSec, toString(source));
  }
  return accepted;
}

void manualSet(int relay, bool on) {
  bool     shouldLog = false;
  LogEntry entry;

  lockState();
  if (app.mode != SystemMode::Manual) {
    unlockState();
    return;
  }

  if (relay == MAIN_RELAY_ID) {
    relaySetMain(on);
  } else if (relay >= 0 && relay < NUM_ZONES) {
    Zone& z    = app.zones[relay];
    bool wasOn = z.relayOn;
    if (on && !wasOn) z.soilBefore = z.soil;

    // Valve ditutup -> catat ke log berapa lama terbuka
    if (!on && wasOn) {
      entry = makeLogEntry(app);
      entry.soilBefore[relay]  = z.soilBefore;
      entry.durationSec[relay] = (millis() - z.relayOnSince) / 1000;
      shouldLog = true;
    }
    relaySetZone(relay, on);
    // Pompa utama mengikuti valve: ON jika ada zona terbuka, OFF jika semua tertutup
    relayFollowZones();
  }
  unlockState();

  if (shouldLog) datalogAdd(entry);
  Serial.printf("[MANUAL] Relay %s -> %s\n",
                relay == MAIN_RELAY_ID ? "utama" : String(relay + 1).c_str(), on ? "ON" : "OFF");
}

bool manualToggle(int relay, String& error) {
  if (relay != MAIN_RELAY_ID && (relay < 0 || relay >= NUM_ZONES)) {
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

void applySettings(const Settings& s) {
  lockState();
  app.settings = s;
  unlockState();

  saveSettings(s);
  // Bangunkan SensorTask agar interval baru langsung berlaku
  if (sensorTaskHandle) xTaskNotifyGive(sensorTaskHandle);
}

void applySchedule(const ScheduleTable& t) {
  lockState();
  app.schedule = t;
  unlockState();

  saveSchedule(t);
}
