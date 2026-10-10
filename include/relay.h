#pragma once
// =============================================================
//  RELAY — satu-satunya tempat yang menulis pin relay.
//  Fungsi relaySet*/relayAllOff mengubah GPIO sekaligus state `app`,
//  jadi WAJIB dipanggil saat memegang lockState().
// =============================================================
#include <Arduino.h>

void relayBegin();                          // set pin output & matikan semua relay

void relaySetZone(uint8_t zone, bool on);   // butuh lockState()
void relaySetMain(bool on);                 // butuh lockState()
void relayAllOff();                         // butuh lockState()

// Semua mode: pompa utama menyala selama ada valve zona yang terbuka.
void relayFollowZones();                    // butuh lockState()
