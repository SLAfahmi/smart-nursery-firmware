// =============================================================
//  OTA CLOUD — update firmware dari server
//
//  Alur (semua di CloudSyncTask, setelah telemetry sukses):
//    1. Balasan telemetry berisi "ota": {version, path, size, md5}
//    2. Tunggu sampai semua relay mati & semua zona Idle
//    3. Unduh CLOUD_BASE_URL + path ke partisi OTA berikutnya, cek MD5
//    4. Catat versi di NVS ("pending"), lalu restart
//    5. Firmware baru berstatus PENDING_VERIFY sampai telemetry pertamanya
//       sukses (otaConfirmBoot). Jika alat restart sebelum itu, bootloader
//       kembali ke firmware lama, dan versi itu ditolak (tidak dicoba lagi).
//
//  Status di telemetry: "ota": {"target", "state", "error"}
//    deferred  menunggu semua relay mati
//    ready     sudah terpasang, restart menunggu zona idle
//    failed    gagal, dicoba lagi setelah OTA_RETRY_INTERVAL_MS
//    rejected  rollback / versi di .bin tidak cocok, tidak dicoba lagi
// =============================================================
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "ota.h"
#include "config.h"
#include "app_state.h"

// Core Arduino biasanya langsung menandai firmware hasil OTA sebagai valid saat boot.
// Ditunda sampai otaConfirmBoot(), supaya firmware yang tidak bisa menghubungi server di-rollback.
extern "C" bool verifyRollbackLater() { return true; }

enum class OtaState : uint8_t { Idle, Deferred, Ready, Failed, Rejected };

// Data firmware yang ditawarkan server
struct OtaRelease {
  String   version;
  String   path;
  String   md5;
  uint32_t size = 0;
};

// -------------------------------------------------------------
//  Konstanta
// -------------------------------------------------------------
static const char* NVS_NAMESPACE  = "ota";
static const char* KEY_PENDING    = "pending";    // versi yang baru dipasang, menunggu konfirmasi
static const char* KEY_REJECTED   = "rejected";   // versi yang pernah gagal
static const char* KEY_REASON     = "reason";     // alasan versi itu gagal
static const int   DOWNLOAD_CHUNK = 4096;         // ukuran potongan unduhan (byte)

// -------------------------------------------------------------
//  Variabel status (hanya diakses CloudSyncTask, jadi tanpa mutex)
// -------------------------------------------------------------
static OtaRelease offered;                    // tawaran terakhir dari server
static bool       hasOffer        = false;
static OtaState   state           = OtaState::Idle;
static String     target;                     // versi yang sedang diproses / dilaporkan
static String     lastError;
static String     rejectedVersion;            // versi yang pernah gagal (NVS), tidak dicoba lagi
static String     rejectedReason;
static uint32_t   lastFailMs      = 0;
static bool       awaitingConfirm = false;    // boot pertama setelah OTA (NVS "pending" = versi ini)
static bool       confirmed       = false;

// =============================================================
//  Helper umum
// =============================================================

static const char* toString(OtaState s) {
  switch (s) {
    case OtaState::Deferred: return "deferred";
    case OtaState::Ready:    return "ready";
    case OtaState::Failed:   return "failed";
    case OtaState::Rejected: return "rejected";
    default:                 return "idle";
  }
}

// Ubah status OTA yang dilaporkan ke server
static void setStatus(OtaState newState, const String& version, const String& error) {
  state     = newState;
  target    = version;
  lastError = error;
}

// getString() mencetak error NVS jika key belum ada, jadi cek dulu
static String readKey(Preferences& prefs, const char* key) {
  return prefs.isKey(key) ? prefs.getString(key) : String();
}

static void nvsPutString(const char* key, const String& value) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putString(key, value);
  prefs.end();
}

static void nvsRemove(const char* key) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.remove(key);
  prefs.end();
}

// Aman untuk update/restart jika tidak ada relay yang menyala dan tidak ada
// zona yang sedang antre, menyiram, atau menunggu air meresap
static bool irrigationIdle() {
  AppState s = copyState();
  if (s.mainRelayOn) return false;
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    if (s.zones[i].relayOn || s.zones[i].state != ZoneState::Idle) return false;
  }
  return true;
}

static void restartNow() {
  Serial.printf("[OTA] Restart ke firmware %s\n", target.c_str());
  Serial.flush();
  vTaskDelay(pdMS_TO_TICKS(200));
  ESP.restart();
}

// =============================================================
//  Saat boot
// =============================================================

// Firmware `version` sudah dipasang, tetapi yang berjalan sekarang versi lain
// (terjadi rollback, atau versi di file .bin salah). Versi itu ditolak permanen.
static void rejectFailedUpdate(Preferences& prefs, const String& version) {
  rejectedVersion = version;
  rejectedReason  = esp_ota_get_last_invalid_partition()
                        ? "rollback: firmware baru restart sebelum terhubung ke server"
                        : "versi di file .bin tidak sama dengan versi rilis";
  prefs.putString(KEY_REJECTED, rejectedVersion);
  prefs.putString(KEY_REASON, rejectedReason);
  prefs.remove(KEY_PENDING);

  setStatus(OtaState::Rejected, rejectedVersion, rejectedReason);
  Serial.printf("[OTA] Update ke %s gagal (%s), versi itu tidak akan dicoba lagi\n",
                rejectedVersion.c_str(), rejectedReason.c_str());
}

void otaBegin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.printf("[OTA] Firmware %s, partisi %s\n", FIRMWARE_VERSION, running ? running->label : "?");

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  rejectedVersion = readKey(prefs, KEY_REJECTED);
  rejectedReason  = readKey(prefs, KEY_REASON);
  String pending  = readKey(prefs, KEY_PENDING);

  if (pending == FIRMWARE_VERSION) {
    awaitingConfirm = true;          // update berhasil boot, tunggu telemetry pertama
  } else if (!pending.isEmpty()) {
    rejectFailedUpdate(prefs, pending);
  }
  prefs.end();
}

// Cek apakah firmware yang berjalan masih menunggu verifikasi (hasil OTA)
static bool isPendingVerify() {
  esp_ota_img_states_t imgState;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &imgState) == ESP_OK &&
         imgState == ESP_OTA_IMG_PENDING_VERIFY;
}

// Dipanggil setiap telemetry sukses; hanya bekerja sekali per boot
void otaConfirmBoot() {
  if (confirmed) return;
  confirmed = true;

  bool pendingVerify = isPendingVerify();
  if (pendingVerify) esp_ota_mark_app_valid_cancel_rollback();

  if (awaitingConfirm) {
    awaitingConfirm = false;
    nvsRemove(KEY_PENDING);
  }

  if (pendingVerify) {
    Serial.printf("[OTA] Firmware %s dikonfirmasi (server terhubung), rollback dibatalkan\n", FIRMWARE_VERSION);
  }
}

// =============================================================
//  Unduh & tulis firmware ke partisi OTA berikutnya
// =============================================================

// Kirim request GET dan pastikan ukuran file sesuai tawaran server.
// Return ukuran file (byte), atau -1 jika gagal (lastError diisi alasannya).
static int startDownload(HTTPClient& http, WiFiClient& client, const String& url, uint32_t expectedSize) {
  http.useHTTP10(true);                      // tanpa chunked encoding: ukuran file harus diketahui
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    lastError = "URL firmware tidak valid";
    return -1;
  }
  http.addHeader("X-Device-Token", DEVICE_KEY);

  int code = http.GET();
  int len  = http.getSize();
  if (code != HTTP_CODE_OK) {
    lastError = "download gagal (HTTP " + String(code) + ")";
    http.end();
    return -1;
  }
  if (len <= 0 || (uint32_t)len != expectedSize) {
    lastError = "ukuran file " + String(len) + " byte, seharusnya " + String(expectedSize);
    http.end();
    return -1;
  }
  return len;
}

// Cetak progres unduhan setiap naik 10%
static void printDownloadProgress(int written, int total, int& loggedPct) {
  int pct = (int)((uint64_t)written * 100 / total);
  if (pct >= loggedPct + 10) {
    loggedPct = pct - pct % 10;
    Serial.printf("[OTA] Mengunduh %d%%\n", loggedPct);
  }
}

// Salin isi stream HTTP ke flash sedikit demi sedikit.
// Return jumlah byte yang berhasil ditulis.
static int writeStreamToFlash(WiFiClient* stream, int totalLen, uint8_t* buf) {
  int      written    = 0;
  int      loggedPct  = 0;
  uint32_t lastDataMs = millis();

  while (written < totalLen) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->read(buf, std::min(avail, DOWNLOAD_CHUNK));
      if (n > 0) {
        if (Update.write(buf, n) != (size_t)n) break;   // gagal tulis flash / bukan image ESP32
        written   += n;
        lastDataMs = millis();
        printDownloadProgress(written, totalLen, loggedPct);
      }
    } else if (!stream->connected() || millis() - lastDataMs > OTA_HTTP_TIMEOUT_MS) {
      break;                                            // koneksi putus / terlalu lama tanpa data
    }
    vTaskDelay(1);   // CloudSyncTask di Core 0: beri giliran task lain & IDLE (watchdog)
  }
  return written;
}

// Return true jika image utuh, MD5 cocok, dan partisi boot sudah dipindah.
// Jika gagal, lastError berisi alasannya (dikirim ke server).
static bool downloadAndFlash(const OtaRelease& rel) {
  String url = String(CLOUD_BASE_URL) + rel.path;

  // 1. Siapkan client. HTTPS tanpa verifikasi sertifikat; keutuhan file dicek lewat MD5.
  //    (pola sama dengan httpPost() di cloud_task.cpp)
  WiFiClientSecure secureClient;
  WiFiClient       plainClient;
  bool isHttps = url.startsWith("https");
  if (isHttps) secureClient.setInsecure();
  WiFiClient& client = isHttps ? static_cast<WiFiClient&>(secureClient) : plainClient;

  // 2. Mulai unduh & cek ukuran file
  HTTPClient http;
  int len = startDownload(http, client, url, rel.size);
  if (len < 0) return false;

  // 3. Siapkan partisi OTA dan buffer
  if (!Update.begin(len, U_FLASH)) {
    lastError = Update.errorString();
    http.end();
    return false;
  }
  Update.setMD5(rel.md5.c_str());            // panjang MD5 sudah dicek di otaOffer()

  uint8_t* buf = (uint8_t*)malloc(DOWNLOAD_CHUNK);
  if (!buf) {
    lastError = "memori tidak cukup";
    Update.abort();
    http.end();
    return false;
  }

  // 4. Tulis firmware ke flash
  int written = writeStreamToFlash(http.getStreamPtr(), len, buf);
  free(buf);
  http.end();

  // 5. Pastikan file lengkap, lalu cek MD5 & pindahkan partisi boot
  if (written != len) {
    if (Update.hasError()) lastError = Update.errorString();
    else                   lastError = "download terputus di " + String(written) + "/" + String(len) + " byte";
    Update.abort();
    return false;
  }
  if (!Update.end()) {
    lastError = Update.errorString();
    return false;
  }
  return true;
}

// =============================================================
//  Tawaran firmware dari server
// =============================================================

// Baca tawaran dari JSON. Return false jika datanya tidak lengkap.
static bool parseOffer(JsonVariantConst offer, OtaRelease& rel) {
  rel.version = offer["version"] | "";
  rel.path    = offer["path"] | "";
  rel.md5     = offer["md5"] | "";
  rel.size    = offer["size"] | 0u;
  rel.md5.toLowerCase();

  return !rel.version.isEmpty() && rel.path.startsWith("/") && rel.md5.length() == 32 && rel.size != 0;
}

// Tidak ada rilis untuk alat ini, atau rilis dibatalkan dari web
static void clearOffer() {
  hasOffer = false;
  if (state == OtaState::Deferred || state == OtaState::Failed) state = OtaState::Idle;
}

// Versi yang pernah gagal: laporkan lagi (juga setelah restart)
// agar server berhenti menawarkan versi ini
static void reportRejected(const String& version) {
  hasOffer = false;
  if (state != OtaState::Rejected || target != version) {
    setStatus(OtaState::Rejected, version, rejectedReason);
    Serial.printf("[OTA] Firmware %s pernah gagal dipasang, diabaikan\n", version.c_str());
  }
}

void otaOffer(JsonVariantConst offer) {
  if (offer.isNull()) {
    clearOffer();
    return;
  }

  OtaRelease rel;
  if (!parseOffer(offer, rel)) {
    Serial.println("[OTA] Tawaran firmware dari server tidak lengkap, diabaikan");
    return;
  }

  if (rel.version == FIRMWARE_VERSION) {     // versi ini sudah terpasang
    hasOffer = false;
    return;
  }
  if (state == OtaState::Ready) return;      // firmware lain sudah terpasang, tinggal restart

  if (rel.version == rejectedVersion) {
    reportRejected(rel.version);
    return;
  }

  // Rilis baru setelah rilis sebelumnya gagal: langsung dicoba, tanpa menunggu jeda
  if (state == OtaState::Failed && rel.version != offered.version) state = OtaState::Idle;
  offered  = rel;
  hasOffer = true;
}

// =============================================================
//  Proses update (dipanggil berkala oleh CloudSyncTask)
// =============================================================

// Masih dalam jeda tunggu setelah update gagal?
static bool isWaitingRetry() {
  return state == OtaState::Failed && millis() - lastFailMs < OTA_RETRY_INTERVAL_MS;
}

// Irigasi masih berjalan: tunda update (pesan Serial hanya dicetak sekali)
static void deferUpdate() {
  if (state != OtaState::Deferred || target != offered.version) {
    Serial.printf("[OTA] Firmware %s tersedia, menunggu semua relay mati\n", offered.version.c_str());
  }
  setStatus(OtaState::Deferred, offered.version, "");
}

// Unduh & pasang firmware yang ditawarkan, lalu restart jika aman
static void installOfferedFirmware() {
  target = offered.version;
  Serial.printf("[OTA] Mengunduh firmware %s (%lu byte)...\n", target.c_str(), (unsigned long)offered.size);
  uint32_t startMs = millis();

  if (!downloadAndFlash(offered)) {
    state      = OtaState::Failed;
    lastFailMs = millis();
    Serial.printf("[OTA] Update gagal: %s. Dicoba lagi dalam %lu menit\n",
                  lastError.c_str(), (unsigned long)(OTA_RETRY_INTERVAL_MS / 60000));
    return;
  }

  // Firmware baru dipakai pada boot berikutnya; dicatat agar boot itu bisa diverifikasi
  nvsPutString(KEY_PENDING, target);
  hasOffer  = false;
  state     = OtaState::Ready;
  lastError = "";
  Serial.printf("[OTA] Firmware %s terpasang (%lu detik)\n", target.c_str(),
                (unsigned long)((millis() - startMs) / 1000));

  // Selama unduhan berjalan, irigasi bisa saja mulai: restart menunggu zona idle lagi
  if (irrigationIdle()) restartNow();
  else Serial.println("[OTA] Restart ditunda sampai semua zona selesai menyiram");
}

void otaService() {
  // Firmware baru sudah terpasang: restart begitu irigasi selesai
  if (state == OtaState::Ready) {
    if (irrigationIdle()) restartNow();
    return;
  }

  if (!hasOffer) return;
  if (isWaitingRetry()) return;

  if (!irrigationIdle()) {
    deferUpdate();
    return;
  }
  installOfferedFirmware();
}

void otaReport(JsonObject telemetry) {
  if (state == OtaState::Idle) return;       // tidak ada proses OTA, tidak perlu dilaporkan

  JsonObject o = telemetry["ota"].to<JsonObject>();
  o["target"] = target;
  o["state"]  = toString(state);
  if (!lastError.isEmpty()) o["error"] = lastError;
}
