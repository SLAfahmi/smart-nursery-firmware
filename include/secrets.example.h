#pragma once
// =============================================================
//  TEMPLATE KREDENSIAL
//  Salin file ini menjadi include/secrets.h lalu isi nilainya.
//  secrets.h ada di .gitignore, jadi tidak ikut ter-commit.
// =============================================================

// WiFi AP (dashboard lokal), minimal 8 karakter
#define AP_PASSWORD        "ganti-password-ap"

// WiFi STA (router berinternet)
#define STA_SSID           "nama-wifi-router"
#define STA_PASSWORD       "password-wifi-router"

// Token alat, harus sama dengan DEVICE_TOKEN di .env server
#define DEVICE_KEY         "ganti-token-alat"

// Login dashboard lokal (Digest Auth)
#define DASHBOARD_USER     "admin"
#define DASHBOARD_PASSWORD "ganti-password-dashboard"
