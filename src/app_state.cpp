#include "app_state.h"
#include <esp_timer.h>

AppState app;

static SemaphoreHandle_t stateMutex = nullptr;
static SemaphoreHandle_t i2cMutex   = nullptr;

void appStateInit() {
  stateMutex = xSemaphoreCreateMutex();
  i2cMutex   = xSemaphoreCreateMutex();
}

void lockState()   { xSemaphoreTake(stateMutex, portMAX_DELAY); }
void unlockState() { xSemaphoreGive(stateMutex); }

AppState copyState() {
  lockState();
  AppState copy = app;
  unlockState();
  return copy;
}

bool lockI2C(uint32_t timeoutMs) {
  return xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void unlockI2C() { xSemaphoreGive(i2cMutex); }

uint32_t uptimeSec() {
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

const char* toString(SystemMode mode) {
  switch (mode) {
    case SystemMode::Auto:     return "auto";
    case SystemMode::Manual:   return "manual";
    case SystemMode::Schedule: return "schedule";
  }
  return "?";
}

const char* toString(ZoneState state) {
  switch (state) {
    case ZoneState::Idle:     return "idle";
    case ZoneState::Queued:   return "queued";
    case ZoneState::Watering: return "watering";
    case ZoneState::Soaking:  return "soaking";
  }
  return "?";
}

const char* toString(TriggerSource source) {
  switch (source) {
    case TriggerSource::None:     return "none";
    case TriggerSource::Fuzzy:    return "fuzzy";
    case TriggerSource::Schedule: return "schedule";
    case TriggerSource::Manual:   return "manual";
  }
  return "?";
}
