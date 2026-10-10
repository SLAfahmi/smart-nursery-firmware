// =============================================================
//  ManualControlTask — pada mode Manual + Timer, matikan relay
//  otomatis setelah durasi timer habis.
// =============================================================
#include "tasks.h"
#include "app_state.h"
#include "controller.h"

void manualControlTask(void* param) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(500));

    AppState s = copyState();
    if (s.mode != SystemMode::Manual || s.settings.manualType != ManualType::Timer) continue;

    uint32_t limitMs = (uint32_t)s.settings.manualDurationSec * 1000;
    uint32_t now     = millis();

    bool anyZoneOpen = false;
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
      if (s.zones[i].relayOn) anyZoneOpen = true;
      if (s.zones[i].relayOn && now - s.zones[i].relayOnSince >= limitMs) {
        Serial.printf("[MANUAL] Timer zona %u habis\n", i + 1);
        manualSet(i, false);
      }
    }
    // Selama ada zona terbuka, pompa mengikuti zona (mati otomatis saat zona terakhir ditutup).
    // Timer pompa hanya berlaku jika pompa dinyalakan sendiri tanpa zona.
    if (!anyZoneOpen && s.mainRelayOn && now - s.mainRelayOnSince >= limitMs) {
      Serial.println("[MANUAL] Timer pompa utama habis");
      manualSet(MAIN_RELAY_ID, false);
    }
  }
}
