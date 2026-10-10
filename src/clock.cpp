#include "clock.h"
#include <RTClib.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include "config.h"
#include "app_state.h"

static RTC_DS3231 rtc;
static volatile bool       rtcOk      = false;
static volatile TimeSource timeSource = TimeSource::None;
static bool                ntpStarted = false;

static const time_t MIN_VALID_EPOCH = 1704067200;   // 2024-01-01, di bawah ini dianggap belum di-set

// ---------- Helper ----------

static void setSystemTime(time_t epoch) {
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
}

// RTC menyimpan waktu lokal (WIB), sistem menyimpan epoch UTC.
static bool writeRtc(time_t epoch) {
  if (!lockI2C()) return false;

  if (!rtcOk) rtcOk = rtc.begin(&Wire);   // coba deteksi ulang (mis. kabel baru terpasang)

  bool written = false;
  if (rtcOk) {
    rtc.adjust(DateTime((uint32_t)(epoch + TZ_OFFSET_SEC)));
    // Verifikasi: baca kembali, selisih harus kecil
    uint32_t readBack = rtc.now().unixtime() - TZ_OFFSET_SEC;
    written = labs((long)readBack - (long)epoch) <= 2;
  }
  unlockI2C();
  return written;
}

// ---------- API ----------

void clockBegin() {
  setenv("TZ", TZ_INFO, 1);
  tzset();

  if (!lockI2C(1000)) return;
  rtcOk = rtc.begin(&Wire);

  bool rtcTimeValid = false;
  time_t epoch = 0;
  if (rtcOk && !rtc.lostPower()) {
    epoch = (time_t)rtc.now().unixtime() - TZ_OFFSET_SEC;
    rtcTimeValid = epoch > MIN_VALID_EPOCH;
  }
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

bool clockSetFromBrowser(time_t epoch) {
  setSystemTime(epoch);            // jam sistem langsung benar walau RTC gagal
  timeSource = TimeSource::Browser;

  bool rtcWritten = writeRtc(epoch);
  Serial.printf("[CLOCK] Sync dari browser: %s (RTC %s)\n",
                clockFormat(epoch).c_str(), rtcWritten ? "tersimpan" : "GAGAL");
  return rtcWritten;
}

void clockServiceNtp() {
  if (!ntpStarted) {
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    ntpStarted = true;
    Serial.println("[CLOCK] NTP dimulai");
    return;
  }

  // Setiap kali NTP selesai sinkron, simpan hasilnya ke RTC
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    timeSource = TimeSource::Ntp;
    bool rtcWritten = writeRtc(time(nullptr));
    Serial.printf("[CLOCK] NTP sinkron: %s (RTC %s)\n",
                  clockFormat(time(nullptr)).c_str(), rtcWritten ? "diperbarui" : "gagal ditulis");
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
