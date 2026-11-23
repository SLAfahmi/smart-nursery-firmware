// =============================================================
//  ScheduleTask — pada mode Schedule, cek jadwal setiap pergantian
//  menit. Irigasi hanya dijalankan jika tanah masih cukup kering.
// =============================================================
#include "tasks.h"
#include "app_state.h"
#include "controller.h"
#include "clock.h"

// Slot jadwal harus dijalankan jika aktif dan jam:menit-nya sama dengan sekarang
static bool isSlotDue(const ScheduleSlot& slot, const struct tm& now) {
  return slot.active && slot.hour == now.tm_hour && slot.minute == now.tm_min;
}

// Jalankan satu slot jadwal: siram hanya jika tanah belum terlalu basah
static void runScheduleSlot(uint8_t zone, const ScheduleSlot& slot, float soil) {
  if (soil > SCHEDULE_SOIL_MAX) {
    Serial.printf("[SCHEDULE] Zona %u %02u:%02u dilewati, tanah %.1f%% > %.0f%%\n",
                  zone + 1, slot.hour, slot.minute, soil, SCHEDULE_SOIL_MAX);
    return;
  }
  if (!requestIrrigation(zone, slot.durationSec, TriggerSource::Schedule)) {
    Serial.printf("[SCHEDULE] Zona %u sedang sibuk, jadwal %02u:%02u dilewati\n",
                  zone + 1, slot.hour, slot.minute);
  }
}

// Periksa semua slot jadwal di semua zona
static void checkAllSchedules(const AppState& s, const struct tm& now) {
  for (uint8_t z = 0; z < NUM_ZONES; z++) {
    for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
      const ScheduleSlot& slot = s.schedule.slot[z][i];
      if (isSlotDue(slot, now)) runScheduleSlot(z, slot, s.zones[z].soil);
    }
  }
}

void scheduleTask(void* param) {
  int lastCheckedMinute = -1;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    struct tm now;
    if (!clockLocal(now)) continue;                 // jam belum valid -> jadwal tidak jalan

    int minuteOfDay = now.tm_hour * 60 + now.tm_min;
    if (minuteOfDay == lastCheckedMinute) continue; // menit ini sudah dicek
    lastCheckedMinute = minuteOfDay;

    AppState s = copyState();
    if (s.mode != SystemMode::Schedule) continue;

    checkAllSchedules(s, now);
  }
}
