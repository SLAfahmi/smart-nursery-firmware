// =============================================================
//  FuzzyTask — dibangunkan SensorTask saat mode Auto.
//  Menghitung durasi siram tiap zona dengan Fuzzy Mamdani, mengecek
//  cooldown, lalu meminta IrrigationTask menyiram.
// =============================================================
#include "tasks.h"
#include "app_state.h"
#include "controller.h"
#include "fuzzy_logic.h"

// Simpan hasil fuzzy ke state agar tampil di dashboard
static void saveFuzzyOutput(uint8_t zone, float duration) {
  lockState();
  app.zones[zone].fuzzyOutput = duration;
  unlockState();
}

// Cooldown: zona tidak boleh disiram ulang terlalu cepat, karena sensor
// tanah butuh waktu untuk merespons air.
// Return true jika zona masih cooldown, minutesLeft diisi sisa waktunya.
static bool isInCooldown(const Zone& zone, uint32_t cooldownSec, uint32_t& minutesLeft) {
  if (zone.lastWateredUptime == 0) return false;   // belum pernah disiram

  uint32_t sinceWatered = uptimeSec() - zone.lastWateredUptime;
  if (sinceWatered >= cooldownSec) return false;

  minutesLeft = (cooldownSec - sinceWatered) / 60;
  return true;
}

// Evaluasi satu zona: hitung fuzzy, lalu minta irigasi jika memang perlu
static void evaluateZone(uint8_t i, const AppState& s) {
  const Zone& zone = s.zones[i];

  // Zona tanpa sensor tanah tidak disiram otomatis
  if (isnan(zone.soil)) {
    saveFuzzyOutput(i, 0);
    Serial.printf(" Z%u: sensor tidak terpasang", i + 1);
    return;
  }

  float duration = prosesFuzzyMamdani(zone.soil, s.sensors.temperature, s.sensors.lux);
  saveFuzzyOutput(i, duration);
  Serial.printf(" Z%u: %.1f%% -> %.0fs", i + 1, zone.soil, duration);
  if (duration <= 0) return;                     // tanah cukup basah
  if (zone.state != ZoneState::Idle) {           // zona sedang diproses
    Serial.print(" (sibuk)");
    return;
  }

  uint32_t minutesLeft = 0;
  if (isInCooldown(zone, s.settings.cooldownSec, minutesLeft)) {
    Serial.printf(" (cooldown %lu mnt)", (unsigned long)minutesLeft);
    return;
  }

  requestIrrigation(i, (uint16_t)lroundf(duration), TriggerSource::Fuzzy);
}

void fuzzyTask(void* param) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     // tunggu dibangunkan SensorTask

    AppState s = copyState();
    if (s.mode != SystemMode::Auto) continue;

    // Jangan menyiram berdasarkan data sensor yang rusak
    if (!s.sensors.dhtOk || !s.sensors.luxOk) {
      Serial.println("[FUZZY] Data DHT22/BH1750 tidak valid, evaluasi dilewati");
      continue;
    }

    Serial.print("[FUZZY]");
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
      evaluateZone(i, s);
    }
    Serial.println();
  }
}
