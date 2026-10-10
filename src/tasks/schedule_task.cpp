// =============================================================
//  ScheduleTask — pada mode Schedule, cek jadwal setiap pergantian
//  menit. Irigasi hanya dijalankan jika tanah masih cukup kering.
// =============================================================
#include "tasks.h"
#include "app_state.h"
#include "controller.h"
#include "clock.h"

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

    for (uint8_t z = 0; z < NUM_ZONES; z++) {
      for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
        const ScheduleSlot& slot = s.schedule.slot[z][i];
        if (!slot.active || slot.hour != now.tm_hour || slot.minute != now.tm_min) continue;

        float soil = s.zones[z].soil;
        if (soil > SCHEDULE_SOIL_MAX) {
          Serial.printf("[SCHEDULE] Zona %u %02u:%02u dilewati, tanah %.1f%% > %.0f%%\n",
                        z + 1, slot.hour, slot.minute, soil, SCHEDULE_SOIL_MAX);
        } else if (!requestIrrigation(z, slot.durationSec, TriggerSource::Schedule)) {
          Serial.printf("[SCHEDULE] Zona %u sedang sibuk, jadwal %02u:%02u dilewati\n",
                        z + 1, slot.hour, slot.minute);
        }
      }
    }
  }
}
