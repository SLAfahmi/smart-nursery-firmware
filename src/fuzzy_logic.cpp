// =============================================================
//  LOGIKA FUZZY MAMDANI
//
//  Batas himpunan dan basis aturan di file ini mengikuti rancangan
//  fuzzy pada laporan TA (simulasi Excel). Jika rancangan diubah,
//  ubah angka batas di bagian 3 dan aturan di bagian 4.
// =============================================================
#include "fuzzy_logic.h"

// =============================================================
//  1. OPERASI FUZZY DASAR
// =============================================================

float fungsiMin(float a, float b) { return (a < b) ? a : b; }

float fungsiMin3(float a, float b, float c) { return fungsiMin(a, fungsiMin(b, c)); }

float fungsiMax(float a, float b) { return (a > b) ? a : b; }

// =============================================================
//  2. BENTUK KURVA KEANGGOTAAN
//  Setiap himpunan fuzzy memakai salah satu dari tiga kurva ini.
// =============================================================

// Kurva linier turun (bahu kiri)
//   μ = 1 jika x <= a, turun linier dari a ke b, μ = 0 jika x >= b
static float kurvaTurun(float x, float a, float b) {
  if (x <= a) return 1.0f;
  if (x > a && x < b) return (b - x) / (b - a);
  return 0.0f;
}

// Kurva linier naik (bahu kanan)
//   μ = 0 jika x <= a, naik linier dari a ke b, μ = 1 jika x >= b
static float kurvaNaik(float x, float a, float b) {
  if (x <= a) return 0.0f;
  if (x > a && x < b) return (x - a) / (b - a);
  return 1.0f;
}

// Kurva trapesium: naik dari a ke b, μ = 1 dari b sampai c, turun dari c ke d.
// Kurva segitiga ditulis sebagai trapesium dengan b = c (puncak di satu titik).
static float kurvaTrapesium(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0.0f;
  if (x > a && x < b) return (x - a) / (b - a);
  if (x >= b && x <= c) return 1.0f;
  if (x > c && x < d) return (d - x) / (d - c);
  return 0.0f;
}

// =============================================================
//  3. FUNGSI KEANGGOTAAN
// =============================================================

// --- Input 1: kelembaban tanah (%) ---
//   Kering : turun     30 - 45
//   Lembab : trapesium 30 - 45 - 50 - 65
//   Basah  : naik      50 - 65
float f_Kering(float y) { return kurvaTurun(y, 30.0f, 45.0f); }
float f_Lembab(float y) { return kurvaTrapesium(y, 30.0f, 45.0f, 50.0f, 65.0f); }
float f_Basah(float y)  { return kurvaNaik(y, 50.0f, 65.0f); }

// --- Input 2: suhu udara (°C) ---
//   Dingin : turun    22 - 26
//   Normal : segitiga 24 - 28 - 32
//   Panas  : naik     30 - 35
float f_Dingin(float x) { return kurvaTurun(x, 22.0f, 26.0f); }
float f_Normal(float x) { return kurvaTrapesium(x, 24.0f, 28.0f, 28.0f, 32.0f); }
float f_Panas(float x)  { return kurvaNaik(x, 30.0f, 35.0f); }

// --- Input 3: cahaya (lux), maksimal 65.000 lux ---
//   Redup  : turun     5.000 - 25.000
//   Sedang : segitiga  5.000 - 35.000 - 65.000
//   Terang : naik     45.000 - 65.000
float f_Redup(float x)  { return kurvaTurun(x, 5000.0f, 25000.0f); }
float f_Sedang(float x) { return kurvaTrapesium(x, 5000.0f, 35000.0f, 35000.0f, 65000.0f); }
float f_Terang(float x) { return kurvaNaik(x, 45000.0f, 65000.0f); }

// --- Output: durasi penyiraman (detik) ---
//   Mati   : turun     0 - 10
//   Pendek : segitiga 10 - 50 - 90
//   Sedang : segitiga 70 - 130 - 190
//   Lama   : naik    170 - 222
float f_Mati(float z)      { return kurvaTurun(z, 0.0f, 10.0f); }
float f_Pendek(float z)    { return kurvaTrapesium(z, 10.0f, 50.0f, 50.0f, 90.0f); }
float f_SedangOut(float z) { return kurvaTrapesium(z, 70.0f, 130.0f, 130.0f, 190.0f); }
float f_Lama(float z)      { return kurvaNaik(z, 170.0f, 222.0f); }

// =============================================================
//  4. FUZZY MAMDANI PROSES & CENTROID DEFUZZIFIKASI
// =============================================================
float prosesFuzzyMamdani(float kelembaban, float suhu, float cahaya) {
  float mu_Kering = f_Kering(kelembaban);
  float mu_Lembab = f_Lembab(kelembaban);
  float mu_Basah = f_Basah(kelembaban);

  float mu_Dingin = f_Dingin(suhu);
  float mu_Normal = f_Normal(suhu);
  float mu_Panas = f_Panas(suhu);

  float mu_Redup = f_Redup(cahaya);
  float mu_Sedang = f_Sedang(cahaya);
  float mu_Terang = f_Terang(cahaya);

  float r_Mati = 0.0f;
  float r_Pendek = 0.0f;
  float r_Sedang = 0.0f;
  float r_Lama = 0.0f;

  // --- Rule Base ---
  // OUTPUT: SEDANG
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Dingin, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Dingin, mu_Sedang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Kering, mu_Normal, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Normal, mu_Terang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Redup));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Sedang));
  r_Sedang = fungsiMax(r_Sedang, fungsiMin3(mu_Lembab, mu_Panas, mu_Terang));

  // OUTPUT: LAMA
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Dingin, mu_Terang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Normal, mu_Sedang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Normal, mu_Terang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Redup));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Sedang));
  r_Lama = fungsiMax(r_Lama, fungsiMin3(mu_Kering, mu_Panas, mu_Terang));

  // OUTPUT: PENDEK
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Redup));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Sedang));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Dingin, mu_Terang));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Normal, mu_Redup));
  r_Pendek = fungsiMax(r_Pendek, fungsiMin3(mu_Lembab, mu_Normal, mu_Sedang));

  // OUTPUT: MATI
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Dingin, mu_Terang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Normal, mu_Terang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Redup));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Sedang));
  r_Mati = fungsiMax(r_Mati, fungsiMin3(mu_Basah, mu_Panas, mu_Terang));

  // --- Defuzzifikasi (Metode Centroid) ---
  float pembilang = 0.0f;
  float penyebut = 0.0f;

  for (float z = 0.0f; z <= 222.0f; z += 1.0f) {
    float m = fungsiMin(f_Mati(z), r_Mati);
    float p = fungsiMin(f_Pendek(z), r_Pendek);
    float s = fungsiMin(f_SedangOut(z), r_Sedang);
    float l = fungsiMin(f_Lama(z), r_Lama);

    float total = fungsiMax(fungsiMax(m, p), fungsiMax(s, l));

    pembilang += z * total;
    penyebut += total;
  }

  if (penyebut == 0.0f)
    return 0.0f;

  float hasil = pembilang / penyebut;

  // safety guard: jika hasil sangat kecil mendekati nol, bulatkan ke 0.
  if (hasil < 10.0f)
    return 0.0f;

  return hasil;
}
