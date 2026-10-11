// =============================================================
//  SensorTask — baca semua sensor, simpan ke state, log berkala,
//  lalu bangunkan FuzzyTask jika mode Auto.
// =============================================================
#include <DHT.h>
#include <BH1750.h>
#include "tasks.h"
#include "app_state.h"
#include "datalog.h"

static DHT    dht(DHT_PIN, DHT22);
static BH1750 lightMeter;

// Kelembaban tanah satu zona dalam persen. rawAdc diisi rata-rata ADC mentah (untuk diagnosa).
static float readSoilPercent(uint8_t zone, float& rawAdc) {
  // Bacaan pertama setelah ganti channel masih membawa muatan channel sebelumnya
  // (kapasitor sampling ADC) -> buang dulu agar zona tidak saling mengikuti.
  analogRead(SOIL_PINS[zone]);
  delayMicroseconds(100);

  // Rata-rata beberapa sampel agar nilai ADC tidak loncat-loncat
  uint32_t sum = 0;
  for (uint8_t i = 0; i < SOIL_SAMPLES; i++) sum += analogRead(SOIL_PINS[zone]);
  float adc = (float)sum / SOIL_SAMPLES;
  rawAdc = adc;

  // 1. Regresi linier (ADC -> nilai moisture analyzer Y)
  float y = SOIL_REG_A + (SOIL_REG_B * adc);

  // 2. Mapping Y (10 - 35) ke persentase (0 - 100%)
  float percent = (y - SOIL_Y_MIN) * (100.0f / SOIL_Y_SPAN);

  // 3. Batasi agar tetap di rentang 0 - 100
  return constrain(percent, 0.0f, 100.0f);
}

static SensorReadings readEnvironment() {
  SensorReadings r;

  r.temperature = dht.readTemperature();
  r.humidity    = dht.readHumidity();
  r.dhtOk       = !isnan(r.temperature) && !isnan(r.humidity);

  // BH1750 berbagi bus I2C dengan RTC -> wajib pakai lockI2C
  float lux = -1;
  if (lockI2C()) {
    lux = lightMeter.readLightLevel();
    unlockI2C();
  }
  r.luxOk = (lux >= 0);              // library mengembalikan nilai negatif saat error
  r.lux   = r.luxOk ? lux : NAN;
  return r;
}

void sensorTask(void* param) {
  dht.begin();
  if (lockI2C(1000)) {
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire)) {
      Serial.println("[SENSOR] BH1750 tidak terdeteksi!");
    }
    unlockI2C();
  }
  vTaskDelay(pdMS_TO_TICKS(2000));   // DHT22 butuh ~2 detik setelah power-on

  bool     firstCycle    = true;
  uint32_t lastLogUptime = 0;

  while (true) {
    // ---- 1. Baca sensor (tanpa lock, karena I/O lambat) ----
    float soil[NUM_ZONES];
    float soilRaw[NUM_ZONES];
    for (uint8_t i = 0; i < NUM_ZONES; i++) soil[i] = readSoilPercent(i, soilRaw[i]);
    SensorReadings env = readEnvironment();

    // ---- 2. Simpan ke state bersama ----
    lockState();
    app.sensors = env;
    for (uint8_t i = 0; i < NUM_ZONES; i++) app.zones[i].soil = soil[i];
    SystemMode mode        = app.mode;
    uint32_t   logInterval = app.settings.logIntervalSec;
    uint16_t   interval    = app.settings.sensorIntervalSec;
    unlockState();

    Serial.printf("[SENSOR] T=%.1f°C H=%.1f%% L=%.0flx | Tanah:", env.temperature, env.humidity, env.lux);
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
      Serial.printf(" Z%u=%.1f%%(adc %.0f)", i + 1, soil[i], soilRaw[i]);
    }
    Serial.println(env.dhtOk ? "" : "  (DHT22 error)");

    // ---- 3. Log rutin (default tiap 1 jam) ----
    uint32_t now = uptimeSec();
    if (firstCycle || now - lastLogUptime >= logInterval) {
      datalogAdd(makeLogEntry(copyState()));
      lastLogUptime = now;
      firstCycle    = false;
    }

    // ---- 4. Picu evaluasi fuzzy ----
    if (mode == SystemMode::Auto && fuzzyTaskHandle) xTaskNotifyGive(fuzzyTaskHandle);

    // Tidur sesuai interval. Bisa dibangunkan lebih awal saat interval diubah dari dashboard.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)interval * 1000));
  }
}
