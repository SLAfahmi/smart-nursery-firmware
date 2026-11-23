#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "config.h"
#include "app_state.h"
#include "relay.h"
#include "clock.h"
#include "settings_store.h"
#include "datalog.h"
#include "web_server.h"
#include "ota.h"
#include "tasks.h"

TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t fuzzyTaskHandle  = nullptr;
TaskHandle_t irrigationTaskHandles[NUM_ZONES] = {};

// =============================================================
//  Diagnosa bus I2C
//  DS3231 = 0x68 (+0x57 EEPROM), BH1750 = 0x23 / 0x5C
// =============================================================

// Cek level idle SDA/SCL. Harus HIGH karena ada pull-up di modul.
static void checkI2cLines() {
  pinMode(I2C_SDA_PIN, INPUT);
  pinMode(I2C_SCL_PIN, INPUT);
  delay(5);

  int sda = digitalRead(I2C_SDA_PIN);
  int scl = digitalRead(I2C_SCL_PIN);
  Serial.printf("[I2C] Level idle tanpa pull-up internal: SDA(GPIO%u)=%d SCL(GPIO%u)=%d\n",
                I2C_SDA_PIN, sda, I2C_SCL_PIN, scl);
  if (!sda || !scl) {
    Serial.println("[I2C] Ada jalur LOW -> modul tidak dapat VCC, tidak ada pull-up, atau jalur short ke GND");
  }
}

// Cetak semua alamat perangkat yang merespons di bus I2C
static void scanI2cDevices() {
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] Perangkat ditemukan di 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.println("[I2C] TIDAK ada perangkat yang merespons. Cek wiring SDA/SCL/VCC/GND.");
}

// =============================================================
//  Langkah inisialisasi
// =============================================================

// Langkah 1: hardware dasar
static void initHardware() {
  appStateInit();
  relayBegin();                      // matikan semua relay secepat mungkin
  analogReadResolution(12);

  checkI2cLines();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  scanI2cDevices();
  Wire.setTimeOut(50);               // jangan sampai bus I2C macet lama
}

// Langkah 2: penyimpanan & konfigurasi
static void initStorage() {
  storageBegin();
  otaBegin();                        // versi firmware & hasil update terakhir (NVS)
  loadSettings(app.settings);        // belum ada task lain, jadi aman tanpa lock
  loadSchedule(app.schedule);
  datalogBegin();
  clockBegin();
}

// Langkah 3: jaringan
static void startWiFi() {
  WiFi.mode(WIFI_AP_STA);
  // Modem sleep menambah latensi ratusan ms per request -> dimatikan agar dashboard responsif
  WiFi.setSleep(false);
  // Reconnect otomatis bawaan terus scan kanal saat router tidak ada, sehingga
  // AP (dashboard) tersendat. Reconnect diatur sendiri oleh CloudSyncTask.
  WiFi.setAutoReconnect(false);

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.begin(STA_SSID, STA_PASSWORD);
  
  Serial.printf("[WIFI] AP \"%s\" aktif -> buka http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.printf("[WIFI] Menghubungkan STA ke \"%s\"...\n", STA_SSID);
}

// =============================================================
//  Pembuatan task FreeRTOS
// =============================================================

// Buat satu task di core tertentu, lalu daftarkan ke MemoryTask
static TaskHandle_t startTask(TaskFunction_t fn, const char* name, uint32_t stackSize,
                              UBaseType_t priority, BaseType_t core, void* param = nullptr) {
  TaskHandle_t handle = nullptr;
  if (xTaskCreatePinnedToCore(fn, name, stackSize, param, priority, &handle, core) != pdPASS) {
    Serial.printf("[BOOT] GAGAL membuat task %s (heap kurang?)\n", name);
    return nullptr;
  }
  memoryWatchTask(handle, stackSize);
  return handle;
}

// Satu IrrigationTask per zona, parameter task = index zona
static void startIrrigationTasks() {
  irrigationBegin();

  static char names[NUM_ZONES][16];
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    snprintf(names[i], sizeof(names[i]), "Irrigation%u", i + 1);
    irrigationTaskHandles[i] = startTask(irrigationTask, names[i], 4096, 3, 1, (void*)(uintptr_t)i);
  }
}

// Langkah 4: semua task
// - Task penerima notifikasi dibuat lebih dulu daripada pengirimnya.
// - Task yang memanggil datalogAdd() (tulis LittleFS) butuh stack >= 4096.
// - Ukuran stack bisa disesuaikan dari laporan MemoryTask di Serial Monitor.
static void startAllTasks() {
  startIrrigationTasks();

  //                           fungsi             nama             stack  prio core
  fuzzyTaskHandle  = startTask(fuzzyTask,         "FuzzyTask",     4096,  4,   1);
  sensorTaskHandle = startTask(sensorTask,        "SensorTask",    4096,  5,   1);
                     startTask(manualControlTask, "ManualTask",    4096,  1,   1);
                     startTask(scheduleTask,      "ScheduleTask",  3072,  1,   1);
                     startTask(memoryTask,        "MemoryTask",    3072,  1,   1);
                     startTask(cloudSyncTask,     "CloudSyncTask", 12288, 2,   0);
}

// =============================================================
//  Program utama
// =============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n===== SMART NURSERY FIRMWARE =====");

  initHardware();
  initStorage();
  startWiFi();
  webServerBegin();
  startAllTasks();

  Serial.println("[BOOT] Semua task berjalan\n");
}

void loop() {
  // Semua pekerjaan ada di task FreeRTOS. Hapus loopTask agar 8 KB stack-nya kembali ke heap.
  vTaskDelete(nullptr);
}
