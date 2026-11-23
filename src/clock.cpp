// =============================================================
//  JAM SISTEM
//  Jam internal ESP32 sebagai sumber waktu utama, RTC DS3231 sebagai
//  cadangan saat boot, dan NTP saat ada internet.
//
//  Catatan: RTC menyimpan waktu LOKAL (WIB), sedangkan jam sistem
//  menyimpan epoch UTC. Konversinya memakai TZ_OFFSET_SEC.
// =============================================================
#include "clock.h"
#include <RTClib.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include "config.h"
#include "app_state.h"

static RTC_DS3231          rtc;
static volatile bool       rtcOk      = false;              // RTC terdeteksi di bus I2C
static volatile TimeSource timeSource = TimeSource::None;   // asal waktu saat ini
static bool                ntpStarted = false;

// -------------------------------------------------------------
//  Helper
// -------------------------------------------------------------

static void setSystemTime(time_t epoch) {
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
}

// Atur zona waktu agar localtime() menghasilkan WIB
static void setTimezone() {
  setenv("TZ", TZ_INFO, 1);
  tzset();
}

// Baca waktu RTC dan ubah ke epoch UTC. Butuh lockI2C().
// Return false jika RTC pernah kehilangan daya atau waktunya belum di-set.
static bool readRtc(time_t& epoch) {
  if (rtc.lostPower()) return false;
  epoch = (time_t)rtc.now().unixtime() - TZ_OFFSET_SEC;
  return epoch > MIN_VALID_EPOCH;
}

// Tulis waktu ke RTC lalu baca kembali untuk memastikan berhasil.
// Return true jika RTC tersimpan dengan benar.
static bool writeRtc(time_t epoch) {
  if (!lockI2C()) return false;

  if (!rtcOk) rtcOk = rtc.begin(&Wire);   // coba deteksi ulang (mis. kabel baru dipasang)

  bool written = false;
  if (rtcOk) {
    rtc.adjust(DateTime((uint32_t)(epoch + TZ_OFFSET_SEC)));
    // Verifikasi: hasil baca kembali boleh selisih maksimal 2 detik
    uint32_t readBack = rtc.now().unixtime() - TZ_OFFSET_SEC;
    written = labs((long)readBack - (long)epoch) <= 2;
  }
  unlockI2C();
  return written;
}

// -------------------------------------------------------------
//  Inisialisasi saat boot
// -------------------------------------------------------------

void clockBegin() {
  setTimezone();

  if (!lockI2C(1000)) return;
  rtcOk = rtc.begin(&Wire);
  time_t epoch        = 0;
  bool   rtcTimeValid = rtcOk && readRtc(epoch);
  unlockI2C();

  if (rtcTimeValid) {
    setSystemTime(epoch);
    timeSource = TimeSource::Rtc;
    Serial.printf("[CLOCK] Waktu dari RTC: %s\n", clockFormat(epoch).c_str());
  } else if (rtcOk) {
    Serial.println("[CLOCK] RTC terdeteksi tetapi waktunya belum di-set (lostPower). Sync dari dashboard/NTP.");
  } else {
    Serial.println("[CLOCK] RTC DS3231 TIDAK terdeteksi. Cek wiring SDA/SCL & alamat 0x68.");
  }
}

// -------------------------------------------------------------
//  Membaca waktu
// -------------------------------------------------------------

bool clockIsValid() { return time(nullptr) > MIN_VALID_EPOCH; }

time_t clockNow() { return time(nullptr); }

bool clockLocal(struct tm& out) {
  if (!clockIsValid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

String clockFormat(time_t epoch) {
  if (epoch <= MIN_VALID_EPOCH) return "-";
  struct tm t;
  localtime_r(&epoch, &t);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

bool clockRtcOk() { return rtcOk; }

TimeSource clockSource() { return timeSource; }

// -------------------------------------------------------------
//  Sinkronisasi waktu
// -------------------------------------------------------------

bool clockSetFromBrowser(time_t epoch) {
  setSystemTime(epoch);            // jam sistem langsung benar walau RTC gagal
  timeSource = TimeSource::Browser;

  bool rtcWritten = writeRtc(epoch);
  Serial.printf("[CLOCK] Sync dari browser: %s (RTC %s)\n",
                clockFormat(epoch).c_str(), rtcWritten ? "tersimpan" : "GAGAL");
  return rtcWritten;
}

// Mulai NTP (cukup sekali, setelah itu berjalan sendiri di background)
static void startNtp() {
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  ntpStarted = true;
  Serial.println("[CLOCK] NTP dimulai");
}

// Simpan waktu hasil NTP ke RTC
static void saveNtpTimeToRtc() {
  timeSource = TimeSource::Ntp;
  bool rtcWritten = writeRtc(time(nullptr));
  Serial.printf("[CLOCK] NTP sinkron: %s (RTC %s)\n",
                clockFormat(time(nullptr)).c_str(), rtcWritten ? "diperbarui" : "gagal ditulis");
}

void clockServiceNtp() {
  if (!ntpStarted) {
    startNtp();
    return;
  }

  // Setiap kali NTP selesai sinkron, simpan hasilnya ke RTC
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    saveNtpTimeToRtc();
  }
}

const char* toString(TimeSource source) {
  switch (source) {
    case TimeSource::None:    return "none";
    case TimeSource::Rtc:     return "rtc";
    case TimeSource::Browser: return "browser";
    case TimeSource::Ntp:     return "ntp";
  }
  return "?";
}
