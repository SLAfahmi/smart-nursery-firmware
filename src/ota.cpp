// =============================================================
//  OTA CLOUD
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

struct OtaRelease {
  String   version;
  String   path;
  String   md5;
  uint32_t size = 0;
};

static const char* NVS_NAMESPACE  = "ota";
static const int   DOWNLOAD_CHUNK = 4096;

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

static const char* toString(OtaState s) {
  switch (s) {
    case OtaState::Deferred: return "deferred";
    case OtaState::Ready:    return "ready";
    case OtaState::Failed:   return "failed";
    case OtaState::Rejected: return "rejected";
    default:                 return "idle";
  }
}

// getString() mencetak error NVS jika key belum ada, jadi cek dulu
static String readKey(Preferences& prefs, const char* key) {
  return prefs.isKey(key) ? prefs.getString(key) : String();
}

// Aman untuk update/restart: tidak ada relay menyala dan tidak ada zona yang
// sedang antre, menyiram, atau menunggu air meresap
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

// -------------------------------------------------------------
//  Boot
// -------------------------------------------------------------
void otaBegin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.printf("[OTA] Firmware %s, partisi %s\n", FIRMWARE_VERSION, running ? running->label : "?");

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  rejectedVersion = readKey(prefs, "rejected");
  rejectedReason  = readKey(prefs, "reason");
  String pending  = readKey(prefs, "pending");

  if (pending == FIRMWARE_VERSION) {
    awaitingConfirm = true;                   // update berhasil boot, tunggu telemetry pertama
  } else if (!pending.isEmpty()) {
    // Firmware `pending` sudah dipasang, tetapi yang berjalan versi lain
    rejectedVersion = pending;
    rejectedReason  = esp_ota_get_last_invalid_partition()
                          ? "rollback: firmware baru restart sebelum terhubung ke server"
                          : "versi di file .bin tidak sama dengan versi rilis";
    prefs.putString("rejected", rejectedVersion);
    prefs.putString("reason", rejectedReason);
    prefs.remove("pending");

    state     = OtaState::Rejected;
    target    = rejectedVersion;
    lastError = rejectedReason;
    Serial.printf("[OTA] Update ke %s gagal (%s), versi itu tidak akan dicoba lagi\n",
                  rejectedVersion.c_str(), rejectedReason.c_str());
  }
  prefs.end();
}

// Dipanggil setiap telemetry sukses; hanya bekerja sekali per boot
void otaConfirmBoot() {
  if (confirmed) return;
  confirmed = true;

  esp_ota_img_states_t imgState;
  bool pendingVerify = esp_ota_get_state_partition(esp_ota_get_running_partition(), &imgState) == ESP_OK &&
                       imgState == ESP_OTA_IMG_PENDING_VERIFY;
  if (pendingVerify) esp_ota_mark_app_valid_cancel_rollback();

  if (awaitingConfirm) {
    awaitingConfirm = false;
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
      prefs.remove("pending");
      prefs.end();
    }
  }

  if (pendingVerify) {
    Serial.printf("[OTA] Firmware %s dikonfirmasi (server terhubung), rollback dibatalkan\n", FIRMWARE_VERSION);
  }
}

// -------------------------------------------------------------
//  Unduh & tulis ke partisi OTA berikutnya
//  Return true jika image utuh, MD5 cocok, dan partisi boot sudah dipindah.
//  Jika gagal, lastError berisi alasannya (dikirim ke server, maks 120 karakter).
// -------------------------------------------------------------
static bool downloadAndFlash(const OtaRelease& rel) {
  String url = String(CLOUD_BASE_URL) + rel.path;

  // Pola client sama dengan httpPost() di cloud_task.cpp
  WiFiClientSecure secureClient;
  WiFiClient       plainClient;
  bool isHttps = url.startsWith("https");
  if (isHttps) secureClient.setInsecure();   // tanpa verifikasi sertifikat; isi file dicek lewat MD5
  WiFiClient& client = isHttps ? static_cast<WiFiClient&>(secureClient) : plainClient;

  HTTPClient http;
  http.useHTTP10(true);                      // tanpa chunked encoding: ukuran file harus diketahui
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    lastError = "URL firmware tidak valid";
    return false;
  }
  http.addHeader("X-Device-Token", DEVICE_KEY);

  int code = http.GET();
  int len  = http.getSize();
  if (code != HTTP_CODE_OK) {
    lastError = "download gagal (HTTP " + String(code) + ")";
    http.end();
    return false;
  }
  if (len <= 0 || (uint32_t)len != rel.size) {
    lastError = "ukuran file " + String(len) + " byte, seharusnya " + String(rel.size);
    http.end();
    return false;
  }

  if (!Update.begin(len, U_FLASH)) {
    lastError = Update.errorString();
    http.end();
    return false;
  }
  Update.setMD5(rel.md5.c_str());            // panjang sudah dicek di otaOffer()

  uint8_t* buf = (uint8_t*)malloc(DOWNLOAD_CHUNK);
  if (!buf) {
    lastError = "memori tidak cukup";
    Update.abort();
    http.end();
    return false;
  }

  WiFiClient* stream     = http.getStreamPtr();
  int         written    = 0;
  int         loggedPct  = 0;
  uint32_t    lastDataMs = millis();
  while (written < len) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->read(buf, std::min(avail, DOWNLOAD_CHUNK));
      if (n > 0) {
        if (Update.write(buf, n) != (size_t)n) break;   // gagal tulis flash / bukan image ESP32
        written   += n;
        lastDataMs = millis();
        int pct = (int)((uint64_t)written * 100 / len);
        if (pct >= loggedPct + 10) {
          loggedPct = pct - pct % 10;
          Serial.printf("[OTA] Mengunduh %d%%\n", loggedPct);
        }
      }
    } else if (!stream->connected() || millis() - lastDataMs > OTA_HTTP_TIMEOUT_MS) {
      break;
    }
    vTaskDelay(1);   // CloudSyncTask di Core 0: beri giliran task lain & IDLE (watchdog)
  }
  free(buf);
  http.end();

  if (written != len) {
    if (Update.hasError()) lastError = Update.errorString();
    else                   lastError = "download terputus di " + String(written) + "/" + String(len) + " byte";
    Update.abort();
    return false;
  }
  if (!Update.end()) {                       // cek MD5, lalu pindahkan partisi boot
    lastError = Update.errorString();
    return false;
  }
  return true;
}

// -------------------------------------------------------------
//  API untuk CloudSyncTask
// -------------------------------------------------------------
void otaOffer(JsonVariantConst offer) {
  if (offer.isNull()) {
    // Tidak ada rilis untuk alat ini, atau rilis dibatalkan dari web
    hasOffer = false;
    if (state == OtaState::Deferred || state == OtaState::Failed) state = OtaState::Idle;
    return;
  }

  OtaRelease rel;
  rel.version = offer["version"] | "";
  rel.path    = offer["path"] | "";
  rel.md5     = offer["md5"] | "";
  rel.size    = offer["size"] | 0u;
  rel.md5.toLowerCase();
  if (rel.version.isEmpty() || !rel.path.startsWith("/") || rel.md5.length() != 32 || rel.size == 0) {
    Serial.println("[OTA] Tawaran firmware dari server tidak lengkap, diabaikan");
    return;
  }

  if (rel.version == FIRMWARE_VERSION) {     // sudah terpasang
    hasOffer = false;
    return;
  }
  if (state == OtaState::Ready) return;      // firmware lain sudah terpasang, tinggal restart

  if (rel.version == rejectedVersion) {
    // Laporkan lagi (juga setelah restart) agar server berhenti menawarkan versi ini
    hasOffer = false;
    if (state != OtaState::Rejected || target != rel.version) {
      state     = OtaState::Rejected;
      target    = rel.version;
      lastError = rejectedReason;
      Serial.printf("[OTA] Firmware %s pernah gagal dipasang, diabaikan\n", rel.version.c_str());
    }
    return;
  }

  // Rilis baru setelah rilis sebelumnya gagal: langsung dicoba, tanpa menunggu jeda
  if (state == OtaState::Failed && rel.version != offered.version) state = OtaState::Idle;
  offered  = rel;
  hasOffer = true;
}

void otaService() {
  if (state == OtaState::Ready) {
    if (irrigationIdle()) restartNow();
    return;
  }
  if (!hasOffer) return;
  if (state == OtaState::Failed && millis() - lastFailMs < OTA_RETRY_INTERVAL_MS) return;

  if (!irrigationIdle()) {
    if (state != OtaState::Deferred || target != offered.version) {
      Serial.printf("[OTA] Firmware %s tersedia, menunggu semua relay mati\n", offered.version.c_str());
    }
    state     = OtaState::Deferred;
    target    = offered.version;
    lastError = "";
    return;
  }

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
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putString("pending", target);
    prefs.end();
  }
  hasOffer  = false;
  state     = OtaState::Ready;
  lastError = "";
  Serial.printf("[OTA] Firmware %s terpasang (%lu detik)\n", target.c_str(),
                (unsigned long)((millis() - startMs) / 1000));

  // Selama unduhan berjalan, irigasi bisa saja mulai: restart menunggu zona idle lagi
  if (irrigationIdle()) restartNow();
  else Serial.println("[OTA] Restart ditunda sampai semua zona selesai menyiram");
}

void otaReport(JsonObject telemetry) {
  if (state == OtaState::Idle) return;
  JsonObject o = telemetry["ota"].to<JsonObject>();
  o["target"] = target;
  o["state"]  = toString(state);
  if (!lastError.isEmpty()) o["error"] = lastError;
}
