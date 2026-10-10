// =============================================================
//  FuzzyTask — dibangunkan SensorTask (mode Auto). Menghitung durasi
//  siram tiap zona dengan Fuzzy Mamdani, cek cooldown, lalu meminta
//  IrrigationTask menyiram.
// =============================================================
#include "tasks.h"
#include "app_state.h"
#include "controller.h"
#include "fuzzy_logic.h"

void fuzzyTask(void* param) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    AppState s = copyState();
    if (s.mode != SystemMode::Auto) continue;

    // Jangan menyiram berdasarkan data sensor yang rusak
    if (!s.sensors.dhtOk || !s.sensors.luxOk) {
      Serial.println("[FUZZY] Data DHT22/BH1750 tidak valid, evaluasi dilewati");
      continue;
    }

    Serial.print("[FUZZY]");
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
      const Zone& zone = s.zones[i];
      float duration = prosesFuzzyMamdani(zone.soil, s.sensors.temperature, s.sensors.lux);

      lockState();
      app.zones[i].fuzzyOutput = duration;
      unlockState();

      Serial.printf(" Z%u: %.1f%% -> %.0fs", i + 1, zone.soil, duration);

      if (duration <= 0) continue;                         // tanah cukup basah
      if (zone.state != ZoneState::Idle) {                 // zona sedang diproses
        Serial.print(" (sibuk)");
        continue;
      }

      // Cooldown: jangan siram ulang terlalu cepat (sensor butuh waktu merespons air)
      bool everWatered = zone.lastWateredUptime != 0;
      uint32_t sinceWatered = uptimeSec() - zone.lastWateredUptime;
      if (everWatered && sinceWatered < s.settings.cooldownSec) {
        Serial.printf(" (cooldown %lu mnt)", (unsigned long)(s.settings.cooldownSec - sinceWatered) / 60);
        continue;
      }

      requestIrrigation(i, (uint16_t)lroundf(duration), TriggerSource::Fuzzy);
    }
    Serial.println();
  }
}
