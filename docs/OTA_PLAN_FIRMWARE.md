# Rencana OTA Cloud — Firmware

> **Status:** rencana, belum diimplementasikan.
> Pasangannya untuk sisi server ada di `docs/OTA_PLAN_SERVER.md`, dengan salinan di
> `C:\HOSTING_SMART_NURSERY_WEB\OTA_PLAN_SERVER.md`.

## Konteks

Saat ini alat hanya bisa di-update lewat kabel USB: `pio run -t upload` untuk firmware dan
`pio run -t uploadfs` untuk dashboard. Dengan OTA cloud, firmware baru beserta dashboard lokalnya bisa
dirilis dari web Laravel. Alat lalu mengunduh dan memasangnya sendiri lewat WiFi STA.

Hasil pengecekan:
- Partisi `default_16MB.csv` sudah punya slot `app0`/`app1` @ 6.25 MB, sedangkan `firmware.bin`
  sekarang ±1.05 MB. **Partisi tidak perlu diubah.**
- Core terpasang adalah arduino-esp32 **2.0.17**. Di sdkconfig ESP32-S3,
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1`, dan core menyediakan hook lemah `verifyRollbackLater()`
  (`cores/esp32/esp32-hal-misc.c`). Rollback otomatis bisa dipakai tanpa mengubah bootloader.
- ESPAsyncWebServer 3.12.1 sudah punya `beginResponse(int, const char*, const uint8_t*, size_t)`.

## Keputusan

1. Tawaran OTA ikut di **balasan telemetry** (tiap 5 detik), tanpa endpoint cek terpisah.
2. **Firmware dan dashboard dirilis dalam satu file.** Dashboard di-embed ke `firmware.bin` (gzip) saat
   build, sehingga satu OTA memperbarui keduanya secara atomik dan rollback ikut melindungi dashboard.
   LittleFS hanya menyimpan data (config, jadwal, log, buffer offline), dan data ini tidak pernah
   ditimpa oleh OTA.
3. Update **menunggu sampai semua relay mati dan semua zona Idle**.

---

## Kontrak API (sama dengan dokumen server)

**Telemetry live (alat → server)** mendapat field tambahan. Field ini hanya ada di telemetry live,
tidak di batch offline:
```json
"firmware_version": "1.0.1",
"ota": { "target": "1.0.2", "state": "deferred|ready|failed|rejected", "error": "..." }
```
- `ota` tidak dikirim saat tidak ada proses OTA.
- Arti setiap `state`:
  - `deferred`: menunggu relay mati.
  - `ready`: sudah di-flash, menunggu zona idle untuk restart.
  - `failed`: gagal, dicoba lagi setelah `OTA_RETRY_INTERVAL_MS`.
  - `rejected`: rollback, atau versi di file tidak cocok. Versi itu tidak akan dicoba lagi.

**Balasan telemetry (server → alat)** mendapat key level atas `ota`, di samping `device_command`. Key ini
sengaja tidak dimasukkan ke dalam `device_command`, karena `device_command` yang tidak null akan memicu
`quickConfirm` terus-menerus.
```json
{ "status": "success", "device_command": null,
  "ota": { "version": "1.0.2", "path": "/api/v1/firmware/7/download", "size": 1071520, "md5": "<32 hex>" } }
```
- `path` dibuat relatif, dan alat menyusun URL-nya sendiri dari `CLOUD_BASE_URL + path`. Cara ini
  menghindari salah skema http/https di belakang proxy hosting, dan tetap cocok dengan mode
  `USE_LOCAL_SERVER`.
- Server mengirim `ota: null` dalam kondisi berikut:
  - tidak ada rilis aktif,
  - versi alat sudah sama dengan rilis,
  - alat belum mengirim `firmware_version` (firmware lama), atau
  - alat melaporkan `rejected` untuk versi itu.

**Download**: `GET /api/v1/firmware/{id}/download` dengan header `X-Device-Token`. Balasannya
`application/octet-stream` dengan `Content-Length` dan header `x-MD5`.

---

## File yang diubah atau dibuat

| File | Perubahan |
|---|---|
| `include/config.h` | Section baru "OTA": `#define FIRMWARE_VERSION "1.0.0"`, `OTA_RETRY_INTERVAL_MS = 10 menit`, `OTA_HTTP_TIMEOUT_MS = 15000` |
| `include/ota.h` (baru) | API modul OTA |
| `src/ota.cpp` (baru) | Download + flash, rollback, status, NVS |
| `src/tasks/cloud_task.cpp` | Kirim `firmware_version`/`ota`, baca `ota` dari balasan, panggil `otaConfirmBoot()` dan `otaService()` |
| `src/main.cpp` | Panggil `otaBegin()` di setup, setelah `storageBegin()` |
| `src/web_server.cpp` | Ganti `serveStatic(LittleFS "/www/")` dengan aset yang di-embed, dan tambah field `firmware` di `/api/data` |
| `data/www/` → `web/` | Folder sumber dashboard dipindah dan folder `data/` dihapus. Tanpa folder itu, `uploadfs` gagal sebelum menulis apa pun, jadi tidak bisa menimpa config/log secara tidak sengaja |
| `web/app.js`, `web/index.html` | Versi firmware tampil di tab Config (dari `/api/data`) |
| `scripts/embed_www.py` (baru) | Pre-build script: gzip `web/*` lalu buat `include/www_assets.h` |
| `platformio.ini` | `extra_scripts = pre:scripts/embed_www.py`, dan komentar `uploadfs` di header dihapus |
| `.gitignore` | Tambah `include/www_assets.h` (file hasil generate) |
| `include/tasks.h`, `README.md` | Tabel task (CloudSyncTask juga menjalankan OTA), section "OTA Cloud", format cloud, langkah upload tanpa `uploadfs` |

---

## Dashboard di-embed ke firmware

Ukurannya kecil: total ±76 KB mentah (app.js 19 KB, index.html 20 KB, style.css 36 KB), atau sekitar
±20 KB setelah gzip, dibandingkan partisi app yang 6.25 MB.

**`scripts/embed_www.py`** adalah pre-script PlatformIO yang berjalan setiap `pio run`:
```python
Import("env")
import gzip, hashlib, os
# Untuk setiap file di web/: gz = gzip.compress(raw, 9, mtime=0)  (mtime=0 -> hasil deterministik)
# etag = '"' + md5(raw)[:16] + '"'; MIME dari ekstensi (.html/.css/.js)
# Tulis include/www_assets.h HANYA jika isinya berubah (agar tidak memicu rebuild penuh)
```

Isi header hasil generate:
```cpp
// AUTO-GENERATED oleh scripts/embed_www.py dari web/ — jangan diedit
struct WebAsset { const char* path; const char* mime; const uint8_t* data; size_t len; const char* etag; };
static const uint8_t WWW_app_js[] = { 0x1f, 0x8b, ... };
static const WebAsset WEB_ASSETS[] = { {"/app.js", "application/javascript", WWW_app_js, 6123, "\"...\""}, ... };
```

**`web_server.cpp`**:
```cpp
// Dashboard tertanam di firmware (gzip). ETag + no-cache: browser selalu revalidasi (balasan 304
// yang ringan), jadi dashboard baru langsung terpakai setelah OTA.
static void sendAsset(AsyncWebServerRequest* request, const WebAsset& a) {
  if (request->hasHeader("If-None-Match") && request->header("If-None-Match") == a.etag) return request->send(304);
  AsyncWebServerResponse* res = request->beginResponse(200, a.mime, a.data, a.len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  res->addHeader("ETag", a.etag);
  request->send(res);
}
// webServerBegin(): satu route per aset, ditambah "/" -> index.html
for (const WebAsset& a : WEB_ASSETS) server.on(a.path, HTTP_GET, [&a](AsyncWebServerRequest* r) { sendAsset(r, a); });
```

File `/www/*` lama di LittleFS tidak dipakai lagi. File itu tidak berbahaya jika dibiarkan, dan tidak
dihapus otomatis.

---

## Modul OTA

### `include/ota.h`
```cpp
void otaBegin();                         // setup(): deteksi rollback / versi tidak cocok (NVS "ota")
void otaConfirmBoot();                   // setelah telemetry sukses: tandai firmware valid (batalkan rollback)
void otaOffer(JsonVariantConst offer);   // simpan tawaran dari balasan server (null = tidak ada / dibatalkan)
void otaService();                       // CloudSyncTask: download+flash jika aman, restart jika siap
void otaReport(JsonObject telemetry);    // isi field "ota" pada telemetry live
```

### `src/ota.cpp`

**Rollback**
- `extern "C" bool verifyRollbackLater() { return true; }`. Dengan ini, firmware hasil OTA tetap
  berstatus `PENDING_VERIFY` sampai `otaConfirmBoot()` dipanggil. Jika alat restart sebelum itu (crash,
  WDT, atau mati listrik), bootloader kembali ke firmware lama.
- `otaConfirmBoot()` dipanggil saat telemetry **pertama** berhasil (2xx). Dengan kriteria ini, firmware
  baru terbukti masih bisa menerima OTA berikutnya. Selama belum dikonfirmasi, irigasi lokal tetap
  berjalan normal.
- Isi `otaConfirmBoot()`:
  1. Cek `esp_ota_get_state_partition(running)`.
  2. Jika statusnya `ESP_OTA_IMG_PENDING_VERIFY`, panggil `esp_ota_mark_app_valid_cancel_rollback()`.
  3. Hapus key NVS `pending`, lalu log `[OTA] Firmware x dikonfirmasi`.
  4. Fungsi ini cukup dijalankan sekali per boot (pakai flag statis).

**NVS** (`Preferences`, namespace `"ota"`)
- `pending`: versi yang baru di-flash, ditulis tepat sebelum restart.
- `rejected`: versi yang gagal.
- Di `otaBegin()`: jika `pending` tidak kosong dan berbeda dari `FIRMWARE_VERSION`, berarti update gagal.
  - Penyebabnya rollback jika `esp_ota_get_last_invalid_partition() != nullptr`. Jika tidak, berarti
    versi di file .bin tidak cocok dengan versi rilis.
  - Versi itu disimpan ke `rejected`, `pending` dihapus, dan state menjadi `Rejected`.
  - Pengecekan ini mencegah **loop update tanpa akhir**.

**`otaOffer()`**
- Memvalidasi `version` (tidak kosong), `path` (diawali `/`), `md5` (32 karakter), dan `size > 0`.
  Isinya lalu disalin ke static `OtaOffer {String version, path, md5; uint32_t size}`, karena dokumen
  JSON-nya bersifat lokal.
- Tawaran diabaikan jika `version == FIRMWARE_VERSION`.
- Jika `version == rejected`, state menjadi `Rejected` supaya server berhenti menawarkan versi itu. Ini
  tetap berlaku setelah restart, karena nilainya dibaca dari NVS.
- Jika server mengirim `null` saat state `Deferred` atau `Failed` (rilis dibatalkan), state kembali ke
  `Idle`.

**`otaService()`** dipanggil setiap siklus telemetry yang sukses:
1. Jika state `Ready` dan `irrigationIdle()` terpenuhi, alat restart.
2. Jika tidak ada tawaran, atau masih dalam jeda setelah gagal, fungsi selesai.
3. Jika `!irrigationIdle()`, state menjadi `Deferred`. Log hanya ditulis saat state berubah.
4. Jalankan `downloadAndFlash()`.
   - Berhasil: tulis `pending = version`. Restart jika masih idle. Jika ada zona yang mulai menyiram
     selama download, state menjadi `Ready`.
   - Gagal: state menjadi `Failed`, `lastError` diisi, dan `lastFailMs = millis()`.

**`irrigationIdle()`** dicek dari `copyState()`. Syaratnya `!mainRelayOn`, dan setiap zona punya
`relayOn == false` dan `state == ZoneState::Idle`.

**`downloadAndFlash()`** memakai pola client yang sama dengan `httpPost()` di cloud_task.cpp
(`WiFiClientSecure::setInsecure()` untuk https, `WiFiClient` untuk http):
```cpp
http.useHTTP10(true);                       // hindari chunked encoding
http.addHeader("X-Device-Token", DEVICE_KEY);
// GET -> wajib 200, getSize() > 0 dan == offer.size
Update.begin(len, U_FLASH); Update.setMD5(offer.md5.c_str());
// loop: baca stream ke buffer heap 4 KB -> Update.write(); log progres tiap 10%;
//       vTaskDelay(1) tiap potongan (CloudSyncTask di Core 0, IDLE0 harus tetap jalan);
//       putus jika !connected() atau tidak ada data > OTA_HTTP_TIMEOUT_MS -> Update.abort()
Update.end();                               // gagal jika MD5 beda -> lastError = Update.errorString()
```
MD5 wajib diambil dari tawaran server. `HTTPUpdate` bawaan sengaja tidak dipakai, karena library itu
melewati verifikasi MD5 jika header `x-MD5` hilang, dan tidak menampilkan progres.

### Integrasi di `cloud_task.cpp`
- Di `buildTelemetry(true)`, dalam blok `includeConfig`, tambahkan:
  `doc["firmware_version"] = FIRMWARE_VERSION; otaReport(doc.as<JsonObject>());`
- Di `handleServerResponse()`, setelah parse berhasil, panggil `otaOffer(doc["ota"]);`
- Di cabang sukses `cloudSyncTask()`:
  - panggil `otaConfirmBoot()` setelah `lastSendUptime`,
  - panggil `otaService()` setelah `flushOffline()`.
- Perbarui komentar di header file supaya mencakup alur OTA.
- Stack CloudSyncTask (12288) tetap, karena TLS sudah berjalan di task ini dan buffer download dialokasikan
  di heap. Pastikan lewat laporan MemoryTask.

### Catatan
- Firmware **pertama** yang punya fitur OTA tetap harus diupload lewat USB (`pio run -t upload`).
  Setelah itu, `uploadfs` tidak perlu lagi.
- HTTPS masih memakai `setInsecure()`, sama seperti telemetry. MD5 hanya melindungi file dari korupsi,
  bukan dari serangan MITM. Pinning CA (`setCACert`) bisa menjadi peningkatan berikutnya.

---

## Prosedur rilis (untuk README)

1. Ubah kode dan/atau dashboard (`web/`), lalu naikkan `FIRMWARE_VERSION` di `include/config.h`.
2. Jalankan `pio run` untuk menghasilkan `.pio/build/esp32-s3-n16r8/firmware.bin`. Dashboard sudah ada
   di dalam file ini.
3. Di web, buka Config → Update Firmware. Isi versi yang **persis sama**, upload file-nya, lalu klik Rilis.
4. Alat mengunduh pada telemetry berikutnya setelah semua zona idle, lalu restart. Firmware dikonfirmasi
   setelah telemetry pertama berhasil. Jika konfirmasi tidak terjadi, alat melakukan rollback dan web
   menampilkan "Ditolak".

## Verifikasi

1. **Build dan upload awal.**
   - `pio run` berhasil, dan `include/www_assets.h` ter-generate.
   - Upload 1.0.0 lewat USB tanpa `uploadfs`. Serial harus menampilkan `[OTA] Firmware 1.0.0, partisi app0`.
   - Buka `http://192.168.4.1`. Dashboard tampil normal, dan DevTools menunjukkan header
     `Content-Encoding: gzip` dan `ETag`. Saat reload, respons aset harus `304`.
   - Config dan jadwal lama di LittleFS masih terbaca.
2. **OTA lokal.** Jalankan `USE_LOCAL_SERVER` + `php artisan serve --host=0.0.0.0`. Build 1.0.1 dengan
   satu perubahan teks di `web/index.html`, lalu rilis di web.
   - Serial menampilkan progres 10%…100%, alat restart ke partisi `app1`, dan muncul pesan "dikonfirmasi".
   - Status di web menjadi "Terpasang".
   - Dashboard lokal langsung menampilkan perubahan teks tadi, dan config, jadwal, serta log tetap utuh.
3. **Keamanan relay.** Di mode Manual, nyalakan relay: status harus "Menunggu relay mati". Setelah relay
   dimatikan, update berjalan.
4. **MD5.** Ubah `md5` di DB. Status harus `failed` dengan pesan MD5, firmware lama tetap jalan, dan
   update dicoba lagi setelah 10 menit.
5. **Rollback.** Rilis 1.0.2 dengan `DEVICE_KEY` salah, sehingga telemetry selalu 401, lalu tekan reset.
   Alat harus kembali ke 1.0.1 dan melaporkan `rejected`, dan server berhenti menawarkan versi itu.
6. **Versi tidak cocok.** Upload .bin versi 1.0.1 dengan label 1.0.3. Alat harus melaporkan `rejected`
   dengan pesan versi tidak cocok, tanpa loop update.
7. **Stack.** Pantau MemoryTask: sisa stack CloudSyncTask selama OTA harus di atas `STACK_WARNING_BYTES`.
