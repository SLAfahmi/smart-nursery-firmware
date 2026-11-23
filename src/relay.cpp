// =============================================================
//  RELAY — satu-satunya tempat yang menulis pin relay.
//  Setiap perubahan pin juga dicatat di state `app`, supaya dashboard
//  dan cloud selalu tahu kondisi relay yang sebenarnya.
// =============================================================
#include "relay.h"
#include "app_state.h"

// Ubah status ON/OFF menjadi level pin (modul relay aktif LOW)
static uint8_t relayLevel(bool on) {
  return on ? RELAY_ON : RELAY_OFF;
}

// Jadikan pin sebagai output dengan kondisi awal OFF.
// Level OFF ditulis dulu sebelum pinMode agar relay tidak sempat "klik" saat boot.
static void setupRelayPin(uint8_t pin) {
  digitalWrite(pin, RELAY_OFF);
  pinMode(pin, OUTPUT);
}

// Cek apakah ada valve zona yang sedang terbuka. Butuh lockState().
static bool isAnyZoneOpen() {
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    if (app.zones[i].relayOn) return true;
  }
  return false;
}

void relayBegin() {
  setupRelayPin(MAIN_RELAY_PIN);
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    setupRelayPin(ZONE_RELAY_PINS[i]);
  }
}

void relaySetZone(uint8_t zone, bool on) {
  if (zone >= NUM_ZONES) return;

  Zone& z = app.zones[zone];
  if (on && !z.relayOn) z.relayOnSince = millis();   // catat waktu saat berubah OFF -> ON
  z.relayOn = on;
  digitalWrite(ZONE_RELAY_PINS[zone], relayLevel(on));
}

void relaySetMain(bool on) {
  if (on && !app.mainRelayOn) app.mainRelayOnSince = millis();
  app.mainRelayOn = on;
  digitalWrite(MAIN_RELAY_PIN, relayLevel(on));
}

void relayAllOff() {
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    relaySetZone(i, false);
  }
  relaySetMain(false);
}

void relayFollowZones() {
  relaySetMain(isAnyZoneOpen());
}
