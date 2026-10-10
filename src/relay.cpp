#include "relay.h"
#include "app_state.h"

void relayBegin() {
  // Tulis level OFF dulu sebelum pinMode agar relay tidak sempat "klik" saat boot
  digitalWrite(MAIN_RELAY_PIN, RELAY_OFF);
  pinMode(MAIN_RELAY_PIN, OUTPUT);

  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    digitalWrite(ZONE_RELAY_PINS[i], RELAY_OFF);
    pinMode(ZONE_RELAY_PINS[i], OUTPUT);
  }
}

void relaySetZone(uint8_t zone, bool on) {
  if (zone >= NUM_ZONES) return;

  Zone& z = app.zones[zone];
  if (on && !z.relayOn) z.relayOnSince = millis();
  z.relayOn = on;
  digitalWrite(ZONE_RELAY_PINS[zone], on ? RELAY_ON : RELAY_OFF);
}

void relaySetMain(bool on) {
  if (on && !app.mainRelayOn) app.mainRelayOnSince = millis();
  app.mainRelayOn = on;
  digitalWrite(MAIN_RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
}

void relayAllOff() {
  for (uint8_t i = 0; i < NUM_ZONES; i++) relaySetZone(i, false);
  relaySetMain(false);
}

void relayFollowZones() {
  bool anyZoneOpen = false;
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    if (app.zones[i].relayOn) anyZoneOpen = true;
  }
  relaySetMain(anyZoneOpen);
}
