// =============================================================
//  SensorTask — baca semua sensor, simpan ke state, catat log
//  berkala, lalu bangunkan FuzzyTask jika mode Auto.
// =============================================================
#include <DHT.h>
#include <BH1750.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include "tasks.h"
#include "app_state.h"
#include "datalog.h"

static DHT    dht(DHT_PIN, DHT22);
static BH1750 lightMeter;

// Pengaturan yang dibaca dari state setiap siklus
struct SensorCycleConfig {
  SystemMode mode;
  uint32_t   logIntervalSec;
  uint16_t   sensorIntervalSec;
};

// Penanda waktu log rutin
static bool     firstCycle    = true;
static uint32_t lastLogUptime = 0;

// =============================================================
//  Sensor kelembaban tanah
// =============================================================

// Cek apakah sensor tanah benar-benar terpasang di pin ini.
// Pin tanpa sensor "mengambang": nilainya ikut pin lain, bukan 0. Dengan pull-down
// internal aktif, pin kosong turun ke ~0 sedangkan sensor tetap menahan tegangannya.
// analogRead() melepas pull sebelum sampling, jadi di sini pakai adc1_get_raw() langsung.
static bool soilConnected(uint8_t pin) {
  gpio_num_t g = (gpio_num_t)pin;
  rtc_gpio_pulldown_en(g);
  gpio_pulldown_en(g);
  delay(2);
  int adc = adc1_get_raw((adc1_channel_t)digitalPinToAnalogChannel(pin));   // SOIL_PINS wajib ADC1
  rtc_gpio_pulldown_dis(g);
  gpio_pulldown_dis(g);
  return adc > SOIL_DISCONNECTED_ADC;
}

// Rata-rata beberapa sampel ADC agar nilainya tidak loncat-loncat
static float readAverageAdc(uint8_t pin) {
  // Bacaan pertama setelah ganti channel masih membawa muatan channel sebelumnya
  // (kapasitor sampling ADC) -> dibuang dulu agar zona tidak saling mengikuti.
  analogRead(pin);
  delayMicroseconds(100);

  uint32_t sum = 0;
  for (uint8_t i = 0; i < SOIL_SAMPLES; i++) {
    sum += analogRead(pin);
  }
  return (float)sum / SOIL_SAMPLES;
}

// Ubah nilai ADC menjadi persen kelembaban tanah (rumus kalibrasi di config.h)
static float adcToSoilPercent(float adc) {
  // 1. Regresi linier: ADC -> nilai moisture analyzer (Y)
  float y = SOIL_REG_A + (SOIL_REG_B * adc);

  // 2. Mapping Y (10 - 35) ke persentase (0 - 100%)
  float percent = (y - SOIL_Y_MIN) * (100.0f / SOIL_Y_SPAN);

  // 3. Batasi agar tetap di rentang 0 - 100
  return constrain(percent, 0.0f, 100.0f);
}

// Kelembaban tanah satu zona dalam persen, NAN jika sensor tidak terpasang.
// rawAdc diisi rata-rata ADC mentah (untuk diagnosa di Serial Monitor).
static float readSoilPercent(uint8_t zone, float& rawAdc) {
  uint8_t pin = SOIL_PINS[zone];
  float   adc = readAverageAdc(pin);
  rawAdc = adc;

  // Dicek setelah sampling agar pull-down tidak mengganggu bacaan sensor.
  // ADC mentok di atas juga tidak valid: pin tersambung ke 3.3V, bukan output sensor.
  if (adc >= SOIL_ADC_MAX_VALID || !soilConnected(pin)) return NAN;

  return adcToSoilPercent(adc);
}

// =============================================================
//  Sensor lingkungan (DHT22 + BH1750)
// =============================================================

// Baca intensitas cahaya. Return nilai negatif jika gagal.
// BH1750 berbagi bus I2C dengan RTC, jadi wajib pakai lockI2C().
static float readLux() {
  float lux = -1;
  if (lockI2C()) {
    lux = lightMeter.readLightLevel();
    unlockI2C();
  }
  return lux;
}

static SensorReadings readEnvironment() {
  SensorReadings r;

  r.temperature = dht.readTemperature();
  r.humidity    = dht.readHumidity();
  r.dhtOk       = !isnan(r.temperature) && !isnan(r.humidity);

  float lux = readLux();
  r.luxOk = (lux >= 0);                // library mengembalikan nilai negatif saat error
  r.lux   = r.luxOk ? lux : NAN;
  return r;
}

static void initSensors() {
  dht.begin();
  if (lockI2C(1000)) {
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire)) {
      Serial.println("[SENSOR] BH1750 tidak terdeteksi!");
    }
    unlockI2C();
  }
  vTaskDelay(pdMS_TO_TICKS(2000));     // DHT22 butuh ~2 detik setelah power-on
}

// =============================================================
//  Langkah-langkah satu siklus
// =============================================================

// Simpan hasil baca ke state bersama, sekaligus ambil pengaturan yang dibutuhkan
static SensorCycleConfig saveReadings(const SensorReadings& env, const float soil[]) {
  SensorCycleConfig cfg;

  lockState();
  app.sensors = env;
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    app.zones[i].soil = soil[i];
  }
  cfg.mode              = app.mode;
  cfg.logIntervalSec    = app.settings.logIntervalSec;
  cfg.sensorIntervalSec = app.settings.sensorIntervalSec;
  unlockState();

  return cfg;
}

static void printReadings(const SensorReadings& env, const float soil[], const float soilRaw[]) {
  Serial.printf("[SENSOR] T=%.1f°C H=%.1f%% L=%.0flx | Tanah:", env.temperature, env.humidity, env.lux);
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    if (isnan(soil[i])) Serial.printf(" Z%u=--(adc %.0f)", i + 1, soilRaw[i]);
    else                Serial.printf(" Z%u=%.1f%%(adc %.0f)", i + 1, soil[i], soilRaw[i]);
  }
  Serial.println(env.dhtOk ? "" : "  (DHT22 error)");
}

// Log rutin (default tiap 1 jam). Siklus pertama setelah boot selalu dicatat.
static void logIfDue(uint32_t logIntervalSec) {
  uint32_t now = uptimeSec();
  if (firstCycle || now - lastLogUptime >= logIntervalSec) {
    datalogAdd(makeLogEntry(copyState()));
    lastLogUptime = now;
    firstCycle    = false;
  }
}

// =============================================================
//  Task
// =============================================================

void sensorTask(void* param) {
  initSensors();

  while (true) {
    // 1. Baca semua sensor (tanpa lock, karena I/O-nya lambat)
    float soil[NUM_ZONES];
    float soilRaw[NUM_ZONES];
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
      soil[i] = readSoilPercent(i, soilRaw[i]);
    }
    SensorReadings env = readEnvironment();

    // 2. Simpan ke state bersama & tampilkan di Serial Monitor
    SensorCycleConfig cfg = saveReadings(env, soil);
    printReadings(env, soil, soilRaw);

    // 3. Log rutin
    logIfDue(cfg.logIntervalSec);

    // 4. Picu evaluasi fuzzy pada mode Auto
    if (cfg.mode == SystemMode::Auto && fuzzyTaskHandle) xTaskNotifyGive(fuzzyTaskHandle);

    // 5. Tidur sesuai interval. Bisa dibangunkan lebih awal saat interval diubah dari dashboard.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)cfg.sensorIntervalSec * 1000));
  }
}
