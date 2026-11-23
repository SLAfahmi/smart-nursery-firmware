#pragma once
// =============================================================
//  LOGIKA FUZZY MAMDANI
//
//  Input  : kelembaban tanah (%), suhu udara (°C), cahaya (lux)
//  Output : durasi penyiraman (detik)
//
//  Tahapan proses:
//    1. Fuzzifikasi   -> hitung derajat keanggotaan setiap input
//    2. Inferensi     -> evaluasi 27 aturan (MIN), gabungkan hasil (MAX)
//    3. Defuzzifikasi -> metode centroid
// =============================================================
#include <Arduino.h>

// --- Operasi fuzzy dasar ---
float fungsiMin(float a, float b);
float fungsiMin3(float a, float b, float c);
float fungsiMax(float a, float b);

// --- Fungsi keanggotaan kelembaban tanah (%) ---
float f_Kering(float y);
float f_Lembab(float y);
float f_Basah(float y);

// --- Fungsi keanggotaan suhu udara (°C) ---
float f_Dingin(float x);
float f_Normal(float x);
float f_Panas(float x);

// --- Fungsi keanggotaan cahaya (lux) ---
float f_Redup(float x);
float f_Sedang(float x);
float f_Terang(float x);

// --- Fungsi keanggotaan output durasi penyiraman (detik) ---
float f_Mati(float z);
float f_Pendek(float z);
float f_SedangOut(float z);
float f_Lama(float z);

// Proses fuzzy lengkap. Return durasi penyiraman dalam detik (0 = tidak perlu disiram).
float prosesFuzzyMamdani(float kelembaban, float suhu, float cahaya);
