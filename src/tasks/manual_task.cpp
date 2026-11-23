// =============================================================
//  ManualControlTask — pada mode Manual + Timer, matikan relay
//  otomatis setelah durasi timer habis.
// =============================================================
#include "tasks.h"
#include "app_state.h"
#include "controller.h"

// Matikan valve zona yang timernya sudah habis.
// Return true jika ada zona yang terbuka saat dicek.
static bool checkZoneTimers(const AppState& s, uint32_t limitMs, uint32_t now) {
  bool anyZoneOpen = false;
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    const Zone& z = s.zones[i];
    if (!z.relayOn) continue;

    anyZoneOpen = true;
    if (now - z.relayOnSince >= limitMs) {
      Serial.printf("[MANUAL] Timer zona %u habis\n", i + 1);
      manualSet(i, false);
    }
  }
  return anyZoneOpen;
}

// Selama ada zona terbuka, pompa mengikuti zona (mati otomatis saat zona terakhir
// ditutup). Timer pompa hanya berlaku jika pompa dinyalakan sendiri tanpa zona.
static void checkMainPumpTimer(const AppState& s, bool anyZoneOpen, uint32_t limitMs, uint32_t now) {
  if (anyZoneOpen || !s.mainRelayOn) return;

  if (now - s.mainRelayOnSince >= limitMs) {
    Serial.println("[MANUAL] Timer pompa utama habis");
    manualSet(MAIN_RELAY_ID, false);
  }
}

void manualControlTask(void* param) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(500));

    AppState s = copyState();
    if (s.mode != SystemMode::Manual || s.settings.manualType != ManualType::Timer) continue;

    uint32_t limitMs = (uint32_t)s.settings.manualDurationSec * 1000;
    uint32_t now     = millis();

    bool anyZoneOpen = checkZoneTimers(s, limitMs, now);
    checkMainPumpTimer(s, anyZoneOpen, limitMs, now);
  }
}
