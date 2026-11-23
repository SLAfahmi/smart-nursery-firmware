// =============================================================
//  LOG HISTORIS
//
//  Circular buffer berisi MAX_LOG_ENTRIES entri terbaru. Jika buffer
//  penuh, entri terlama ditimpa. Buffer berada di PSRAM, dan setiap
//  ada entri baru seluruh isinya disalin ke file /logs.bin.
//
//  Format file: [LogFileHeader][LogEntry][LogEntry]... (urut terlama -> terbaru)
// =============================================================
#include "datalog.h"
#include <LittleFS.h>
#include "clock.h"

static const char*    LOG_FILE  = "/logs.bin";
static const uint32_t LOG_MAGIC = 0x4C4F4731;   // "LOG1", ganti jika struct LogEntry berubah

struct LogFileHeader {
  uint32_t magic;
  uint16_t entrySize;
  uint16_t count;
};

static LogEntry*         entries  = nullptr;   // circular buffer (PSRAM)
static uint16_t          head     = 0;         // posisi tulis berikutnya
static uint16_t          count    = 0;         // jumlah entri yang terisi
static SemaphoreHandle_t logMutex = nullptr;

// -------------------------------------------------------------
//  Helper
// -------------------------------------------------------------

static void lockLog()   { xSemaphoreTake(logMutex, portMAX_DELAY); }
static void unlockLog() { xSemaphoreGive(logMutex); }

// Posisi di buffer untuk entri ke-i (i = 0 adalah entri terlama)
static uint16_t indexOf(uint16_t i) {
  return (head + MAX_LOG_ENTRIES - count + i) % MAX_LOG_ENTRIES;
}

// Alokasi buffer di PSRAM. Jika gagal, coba di RAM internal.
static LogEntry* allocateBuffer(size_t bytes) {
  LogEntry* buf = (LogEntry*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (LogEntry*)malloc(bytes);
  return buf;
}

// Header file valid jika magic dan ukuran entri sama dengan versi firmware ini
static bool isHeaderValid(const LogFileHeader& header) {
  return header.magic == LOG_MAGIC && header.entrySize == sizeof(LogEntry);
}

// -------------------------------------------------------------
//  File logs.bin
// -------------------------------------------------------------

static void saveToFile() {
  File f = LittleFS.open(LOG_FILE, "w");
  if (!f) return;

  LogFileHeader header = {LOG_MAGIC, sizeof(LogEntry), count};
  f.write((const uint8_t*)&header, sizeof(header));
  for (uint16_t i = 0; i < count; i++) {
    f.write((const uint8_t*)&entries[indexOf(i)], sizeof(LogEntry));
  }
  f.close();
}

static void loadFromFile() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return;

  LogFileHeader header;
  bool headerRead = f.read((uint8_t*)&header, sizeof(header)) == sizeof(header);
  if (!headerRead || !isHeaderValid(header)) {
    f.close();
    Serial.println("[LOG] Format logs.bin lama/rusak, log dikosongkan");
    return;
  }

  uint16_t toRead = min<uint16_t>(header.count, MAX_LOG_ENTRIES);
  for (uint16_t i = 0; i < toRead; i++) {
    if (f.read((uint8_t*)&entries[i], sizeof(LogEntry)) != sizeof(LogEntry)) break;
    count++;
  }
  head = count % MAX_LOG_ENTRIES;
  f.close();
}

// -------------------------------------------------------------
//  API
// -------------------------------------------------------------

bool datalogBegin() {
  logMutex = xSemaphoreCreateMutex();

  size_t bytes = sizeof(LogEntry) * MAX_LOG_ENTRIES;
  entries = allocateBuffer(bytes);
  if (!entries) {
    Serial.println("[LOG] Gagal alokasi buffer log!");
    return false;
  }

  loadFromFile();
  Serial.printf("[LOG] %u entri dimuat (%u byte buffer)\n", count, (unsigned)bytes);
  return true;
}

void datalogAdd(const LogEntry& entry) {
  if (!entries) return;

  lockLog();
  entries[head] = entry;
  head = (head + 1) % MAX_LOG_ENTRIES;
  if (count < MAX_LOG_ENTRIES) count++;
  saveToFile();
  unlockLog();
}

void datalogClear() {
  lockLog();
  head  = 0;
  count = 0;
  LittleFS.remove(LOG_FILE);
  unlockLog();
}

size_t datalogCount() { return count; }

void datalogForEach(size_t maxCount, const std::function<void(const LogEntry&)>& fn) {
  if (!entries) return;

  lockLog();
  uint16_t n     = min<size_t>(maxCount, count);   // jumlah entri yang diambil
  uint16_t start = count - n;                      // lewati entri yang lebih lama
  for (uint16_t i = start; i < count; i++) {
    fn(entries[indexOf(i)]);
  }
  unlockLog();
}

LogEntry makeLogEntry(const AppState& state) {
  LogEntry e    = {};
  e.timestamp   = clockIsValid() ? (uint32_t)clockNow() : 0;
  e.temperature = state.sensors.temperature;
  e.humidity    = state.sensors.humidity;
  e.lux         = state.sensors.lux;
  e.systemMode  = (uint8_t)state.mode;
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    e.soilBefore[i]  = state.zones[i].soil;
    e.soilAfter[i]   = state.zones[i].soil;
    e.durationSec[i] = 0;
  }
  return e;
}
