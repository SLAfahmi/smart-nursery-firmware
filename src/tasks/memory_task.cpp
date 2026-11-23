// =============================================================
//  MemoryTask — laporan pemakaian memori ke Serial Monitor SAJA.
//
//  Cara membaca laporan:
//   - Heap internal "min" terus turun      -> ada memory leak
//   - "largest block" kecil (< 20 KB)       -> heap terfragmentasi, koneksi
//     HTTPS (butuh blok besar) & web bisa lambat/gagal
//   - Stack "sisa" mendekati 0              -> task hampir stack overflow,
//     naikkan ukuran stack task tersebut di main.cpp
// =============================================================
#include <WiFi.h>
#include "tasks.h"
#include "app_state.h"

// Task yang dipantau beserta ukuran stack-nya
struct WatchedTask {
  TaskHandle_t handle;
  uint32_t     stackSize;
};

static const uint8_t  MAX_WATCHED           = 16;
static const uint32_t INTERNAL_HEAP_CAPS    = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static const uint32_t FRAGMENTED_BLOCK_SIZE = 20 * 1024;   // largest block di bawah ini = terfragmentasi

static WatchedTask watched[MAX_WATCHED];
static uint8_t     watchedCount = 0;

void memoryWatchTask(TaskHandle_t handle, uint32_t stackSize) {
  if (handle && watchedCount < MAX_WATCHED) watched[watchedCount++] = {handle, stackSize};
}

// -------------------------------------------------------------
//  Bagian-bagian laporan
// -------------------------------------------------------------

static void printHeapInfo() {
  Serial.printf("Heap internal : free %6.1f KB | min %6.1f KB | largest block %6.1f KB | total %6.1f KB\n",
                heap_caps_get_free_size(INTERNAL_HEAP_CAPS) / 1024.0f,
                heap_caps_get_minimum_free_size(INTERNAL_HEAP_CAPS) / 1024.0f,
                heap_caps_get_largest_free_block(INTERNAL_HEAP_CAPS) / 1024.0f,
                heap_caps_get_total_size(INTERNAL_HEAP_CAPS) / 1024.0f);
}

static void printPsramInfo() {
  if (!psramFound()) {
    Serial.println("PSRAM         : TIDAK terdeteksi (cek setting board / memory_type)");
    return;
  }
  Serial.printf("PSRAM         : free %6.1f KB | min %6.1f KB | total %6.1f KB\n",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0f,
                heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024.0f,
                heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024.0f);
}

static void printFragmentationWarning() {
  if (heap_caps_get_largest_free_block(INTERNAL_HEAP_CAPS) < FRAGMENTED_BLOCK_SIZE) {
    Serial.println("PERINGATAN    : heap internal terfragmentasi, HTTPS & web bisa lambat");
  }
}

// Info jaringan singkat, membantu membedakan lambat karena memori atau karena WiFi
static void printWiFiInfo() {
  bool staConnected = WiFi.status() == WL_CONNECTED;
  Serial.printf("WiFi          : STA %s (RSSI %d dBm) | AP client %u\n",
                staConnected ? "terhubung" : "putus",
                staConnected ? WiFi.RSSI() : 0, WiFi.softAPgetStationNum());
}

// Cetak satu baris tabel stack. stackSize = 0 jika ukurannya tidak diketahui.
static void printTaskStack(TaskHandle_t handle, uint32_t stackSize) {
  // Di ESP32, high water mark = sisa stack minimum dalam BYTE
  uint32_t    freeMin = uxTaskGetStackHighWaterMark(handle);
  const char* warn    = freeMin < STACK_WARNING_BYTES ? "  <-- HAMPIR PENUH!" : "";

  if (stackSize > 0) {
    uint32_t usedMax = stackSize - freeMin;
    Serial.printf("  %-16s %6lu %6lu %6lu  %3lu%%%s\n", pcTaskGetName(handle),
                  (unsigned long)stackSize, (unsigned long)usedMax, (unsigned long)freeMin,
                  (unsigned long)(usedMax * 100 / stackSize), warn);
  } else {
    Serial.printf("  %-16s %6s %6s %6lu     -%s\n", pcTaskGetName(handle), "?", "?",
                  (unsigned long)freeMin, warn);
  }
}

static void printStackTable() {
  Serial.println("Stack task         ukuran  pakai   sisa  pakai%");
  for (uint8_t i = 0; i < watchedCount; i++) {
    printTaskStack(watched[i].handle, watched[i].stackSize);
  }

  // Task milik library web server (ukuran stack ditentukan library)
  TaskHandle_t asyncTcp = xTaskGetHandle("async_tcp");
  if (asyncTcp) printTaskStack(asyncTcp, 0);
}

static void printReport() {
  Serial.printf("\n========== MEMORY REPORT (uptime %lu s) ==========\n", (unsigned long)uptimeSec());
  printHeapInfo();
  printPsramInfo();
  printFragmentationWarning();
  printWiFiInfo();
  printStackTable();
  Serial.println("==================================================\n");
}

// -------------------------------------------------------------
//  Task
// -------------------------------------------------------------

void memoryTask(void* param) {
  vTaskDelay(pdMS_TO_TICKS(5000));   // beri waktu semua task & WiFi selesai start
  while (true) {
    printReport();
    vTaskDelay(pdMS_TO_TICKS(MEMORY_REPORT_INTERVAL_MS));
  }
}
