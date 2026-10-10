// =============================================================
//  MemoryTask — laporan pemakaian memori ke Serial Monitor SAJA.
//
//  Cara membaca laporan:
//   - Heap internal "min" terus turun      -> ada memory leak
//   - "largest block" kecil (< 20 KB)       -> heap terfragmentasi, koneksi
//     HTTPS (butuh blok besar) & web bisa lambat/gagal
//   - Stack "sisa min" mendekati 0          -> task hampir stack overflow,
//     naikkan ukuran stack task tersebut di main.cpp
// =============================================================
#include <WiFi.h>
#include "tasks.h"
#include "app_state.h"

struct WatchedTask {
  TaskHandle_t handle;
  uint32_t     stackSize;
};

static const uint8_t MAX_WATCHED = 16;
static WatchedTask   watched[MAX_WATCHED];
static uint8_t       watchedCount = 0;

void memoryWatchTask(TaskHandle_t handle, uint32_t stackSize) {
  if (handle && watchedCount < MAX_WATCHED) watched[watchedCount++] = {handle, stackSize};
}

static void printTaskStack(TaskHandle_t handle, uint32_t stackSize) {
  // Di ESP32, high water mark = sisa stack minimum dalam BYTE
  uint32_t freeMin = uxTaskGetStackHighWaterMark(handle);
  const char* warn = freeMin < STACK_WARNING_BYTES ? "  <-- HAMPIR PENUH!" : "";

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

static void printReport() {
  const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

  Serial.printf("\n========== MEMORY REPORT (uptime %lu s) ==========\n", (unsigned long)uptimeSec());

  Serial.printf("Heap internal : free %6.1f KB | min %6.1f KB | largest block %6.1f KB | total %6.1f KB\n",
                heap_caps_get_free_size(internalCaps) / 1024.0f,
                heap_caps_get_minimum_free_size(internalCaps) / 1024.0f,
                heap_caps_get_largest_free_block(internalCaps) / 1024.0f,
                heap_caps_get_total_size(internalCaps) / 1024.0f);

  if (psramFound()) {
    Serial.printf("PSRAM         : free %6.1f KB | min %6.1f KB | total %6.1f KB\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0f,
                  heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024.0f,
                  heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024.0f);
  } else {
    Serial.println("PSRAM         : TIDAK terdeteksi (cek setting board / memory_type)");
  }

  if (heap_caps_get_largest_free_block(internalCaps) < 20 * 1024) {
    Serial.println("PERINGATAN    : heap internal terfragmentasi, HTTPS & web bisa lambat");
  }

  // Info jaringan singkat — membantu membedakan lambat karena memori atau WiFi
  Serial.printf("WiFi          : STA %s (RSSI %d dBm) | AP client %u\n",
                WiFi.status() == WL_CONNECTED ? "terhubung" : "putus",
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, WiFi.softAPgetStationNum());

  Serial.println("Stack task         ukuran  pakai   sisa  pakai%");
  for (uint8_t i = 0; i < watchedCount; i++) printTaskStack(watched[i].handle, watched[i].stackSize);

  // Task milik library web server (ukuran stack ditentukan library)
  TaskHandle_t asyncTcp = xTaskGetHandle("async_tcp");
  if (asyncTcp) printTaskStack(asyncTcp, 0);

  Serial.println("==================================================\n");
}

void memoryTask(void* param) {
  vTaskDelay(pdMS_TO_TICKS(5000));   // beri waktu semua task & WiFi selesai start
  while (true) {
    printReport();
    vTaskDelay(pdMS_TO_TICKS(MEMORY_REPORT_INTERVAL_MS));
  }
}
