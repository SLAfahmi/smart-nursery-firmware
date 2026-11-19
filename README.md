# Smart Nursery Firmware (ESP32-S3 N16R8)

Irigasi otomatis dengan 3 mode: **Auto (Fuzzy Mamdani)**, **Manual**, dan **Schedule**.
Dashboard lokal tersedia lewat WiFi AP, dan data dikirim ke server Laravel lewat WiFi STA.

## Cara upload

1. Salin `include/secrets.example.h` menjadi `include/secrets.h`, lalu isi `AP_PASSWORD`, `STA_SSID`, `STA_PASSWORD`, `DEVICE_KEY` (sama dengan `DEVICE_TOKEN` di `.env` server), `DASHBOARD_USER`, dan `DASHBOARD_PASSWORD`. File ini ada di `.gitignore`, jadi kredensial tidak ikut ter-commit. Tanpa file ini build berhenti dengan pesan error.
2. Di `include/config.h`, `CLOUD_URL` dan `CLOUD_BATCH_URL` dibentuk dari `CLOUD_BASE_URL` (server hosting).
3. Upload firmware: `pio run -t upload`. Dashboard (folder `web/`) ikut tertanam di firmware, jadi tidak ada langkah `uploadfs`.
4. Buka Serial Monitor: `pio device monitor`
5. Sambungkan HP ke WiFi `SmartNursery-TelU`, lalu buka `http://192.168.4.1` dan login dengan `DASHBOARD_USER` / `DASHBOARD_PASSWORD`.

> Upload lewat USB (`pio run -t upload`) tetap bisa dipakai kapan saja, dan sekarang sekaligus memasang dashboard. USB hanya *wajib* untuk firmware pertama yang punya fitur OTA. Setelah itu, rilis bisa lewat USB atau cloud, lihat [OTA Cloud](#ota-cloud).
>
> Yang tidak dipakai lagi hanya `uploadfs`. Perintah itu menimpa seluruh LittleFS (config, jadwal, log), jadi jangan membuat folder `data/` lagi. Tanpa folder itu, `pio run -t uploadfs` gagal sebelum menulis apa pun ke alat.

## OTA Cloud

Firmware baru, termasuk dashboard di `web/`, bisa dirilis dari web tanpa kabel USB.

1. Naikkan `FIRMWARE_VERSION` di `include/config.h` (mis. `1.0.0` → `1.0.1`).
2. Build dengan `pio run`. Hasilnya ada di `.pio/build/esp32-s3-n16r8/firmware.bin`.
3. Di web, buka tab Config → Update Firmware. Isi versi yang **sama persis** dengan `FIRMWARE_VERSION`, pilih `firmware.bin`, lalu klik Rilis.
4. Alat menerima tawaran itu di balasan telemetry berikutnya. Alat menunggu sampai semua relay mati dan semua zona idle, lalu mengunduh, memasang, dan restart. Selama mengunduh (±30–60 detik) telemetry berhenti, jadi web menampilkan alat offline sebentar.
5. Firmware baru dianggap valid setelah telemetry pertamanya berhasil. Jika alat restart sebelum itu (crash, listrik mati, atau tidak bisa terhubung ke server), bootloader kembali ke firmware lama dan versi itu ditandai **ditolak**, sehingga tidak dicoba lagi. Untuk mencoba lagi, perbaiki lalu rilis dengan nomor versi baru.

Catatan:
- Partisi `default_16MB.csv` sudah menyediakan dua slot aplikasi (6.25 MB) untuk OTA. Jangan ganti ke tabel partisi tanpa slot OTA.
- OTA tidak menyentuh isi LittleFS (config, jadwal, log, buffer offline).
- Download memakai HTTPS tanpa verifikasi sertifikat, sama seperti telemetry. Keutuhan file dicek dengan MD5.
- Log OTA di Serial Monitor diawali `[OTA]`.

## Struktur

```
include/config.h          semua pin, konstanta & default
include/secrets.h         kredensial WiFi, token alat, login dashboard (tidak di-commit; template: secrets.example.h)
include/types.h           enum & struct bersama (Zone, Settings, LogEntry, ...)
src/main.cpp              inisialisasi hardware, WiFi, pembuatan task
src/app_state.cpp         state bersama + mutex (lockState / lockI2C)
src/controller.cpp        aksi sistem: ganti mode, minta irigasi, kontrol manual
src/relay.cpp             satu-satunya tempat yang menulis pin relay
src/clock.cpp             jam sistem, RTC DS3231, NTP
src/fuzzy_logic.cpp       Fuzzy Mamdani (sama dengan referensi)
src/settings_store.cpp    simpan/baca config.json & schedule.json (LittleFS)
src/datalog.cpp           log historis 100 entri (buffer di PSRAM, file logs.bin)
src/web_server.cpp        REST API + file dashboard (ESPAsyncWebServer)
src/ota.cpp               OTA cloud: unduh firmware, rollback, status update
src/tasks/*.cpp           satu file per task FreeRTOS
web/                      dashboard: index.html, style.css, app.js (ditanam ke firmware saat build)
scripts/embed_www.py      pre-build: gzip web/* -> include/www_assets.h (file hasil generate)
```

## Task FreeRTOS

| Task | Core | Prio | Stack | Fungsi |
|---|---|---|---|---|
| SensorTask | 1 | 5 | 4096 | baca sensor, log berkala, picu FuzzyTask |
| FuzzyTask | 1 | 4 | 4096 | hitung durasi siram tiap zona, cek cooldown |
| Irrigation1..3 | 1 | 3 | 3072 | eksekusi siram satu zona |
| ManualTask | 1 | 1 | 3072 | matikan relay saat timer manual habis |
| ScheduleTask | 1 | 1 | 3072 | picu irigasi sesuai jadwal |
| MemoryTask | 1 | 1 | 3072 | laporan heap & stack ke Serial Monitor (tidak ke web) |
| CloudSyncTask | 0 | 2 | 12288 | telemetry, buffer offline, NTP, reconnect WiFi, OTA |
| async_tcp (library) | 1 | 10 | 16384 | melayani web server (pengganti WebTask) |

## Perbaikan web lambat & RTC

**Web lambat**
- Web server diganti ke `ESPAsyncWebServer` (event-driven), jadi tidak perlu task yang polling `handleClient()`.
- `WiFi.setSleep(false)`: modem sleep menambah latensi ratusan ms per request.
- Auto-reconnect STA dimatikan. Saat router tidak ada, reconnect bawaan terus scan kanal dan membuat AP tersendat. Reconnect sekarang dicoba tiap 20 detik oleh CloudSyncTask.
- TLS/HTTPS (CloudSyncTask) berjalan di Core 0, web server di Core 1.
- Dashboard: polling hanya mengirim request baru setelah request sebelumnya selesai, setiap request punya timeout, dan polling berhenti saat tab tidak aktif. File dashboard dikirim dalam bentuk gzip dari flash program dengan `ETag`, jadi browser hanya mengunduh ulang file yang berubah (selain itu cukup dibalas `304`).
- Tidak ada I/O (I2C, file, jaringan) selama `lockState()` dipegang, jadi handler web tidak pernah menunggu lama.

**RTC tidak bisa di-sync**
- BH1750 dan DS3231 berbagi bus I2C tetapi sebelumnya diakses dari task berbeda tanpa pengaman, sehingga transaksinya bisa bertabrakan. Sekarang semua akses I2C memakai `lockI2C()`.
- Task membaca waktu dari jam internal ESP32, bukan dari RTC. RTC hanya dibaca saat boot dan ditulis saat sync, lalu hasil tulis diverifikasi dengan membacanya kembali.
- Sync dari browser mengirim epoch. Jika RTC gagal ditulis, jam sistem tetap diperbarui dan dashboard menampilkan alasannya.
- Jika internet tersedia, waktu diambil otomatis dari NTP lalu disimpan ke RTC.
- Status RTC (terdeteksi atau tidak) tampil di tab Config.

## REST API lokal

Semua route (halaman dashboard dan API) memakai **Digest Auth** dengan `DASHBOARD_USER` / `DASHBOARD_PASSWORD`. Request `POST` juga wajib membawa header `X-Requested-With` (nilai bebas) sebagai proteksi CSRF; tanpa header itu balasannya `403`. `app.js` sudah mengirim header ini. Contoh dengan curl:

```
curl --digest -u admin:PASSWORD -X POST -H "X-Requested-With: curl" "http://192.168.4.1/api/relay?relay=1"
```

| Endpoint | Method | Fungsi |
|---|---|---|
| `/api/data` | GET | sensor, zona, relay, waktu, status WiFi/cloud, versi firmware |
| `/api/override?mode=0\|1\|2` | POST | ganti mode sistem |
| `/api/relay?relay=main\|1..3` | POST | toggle relay (mode Manual) |
| `/api/config` | GET/POST | `sensorInterval`, `manualMode`, `manualDuration`, `logInterval`, `cooldown` |
| `/api/schedule` | GET/POST | `[{"zone":1,"slots":[{"active":true,"hour":6,"minute":0,"duration":60}]}]` |
| `/api/log?limit=N` | GET | log historis (JSON) |
| `/api/log.csv` | GET | unduh log CSV |
| `/api/log/reset` | POST | hapus log |
| `/api/sync-time` | POST | `{"epoch": 1760000000}` |

## Format cloud

Acuan lengkap: `RINGKASAN_DAN_REVISI.md` bagian 6 di repo web.

- Header: `X-Device-Token: DEVICE_KEY`. Token salah → HTTP 401.
- Telemetry tiap 5 detik (`CLOUD_URL`):
  ```
  {"recorded_at","temperature","humidity","light","system_mode","relay_main",
   "zones":[{"zone","soil","relay","state","fuzzy_duration"}],
   "command_version","config","schedules","firmware_version","ota"}
  ```
  - `recorded_at` waktu lokal WIB `YYYY-MM-DD HH:MM:SS`, atau `null` jika jam belum valid.
  - Nilai sensor `null` jika error / sensor tanah tidak terpasang.
  - `command_version`, `config` (format `/api/config`), `schedules` (format `/api/schedule`), `firmware_version`, dan `ota` hanya dikirim pada telemetry live.
  - `ota` berisi `{"target","state","error"}` dan hanya dikirim selama ada proses OTA. Nilai `state`: `deferred` (menunggu relay mati), `ready` (sudah terpasang, menunggu restart), `failed` (dicoba lagi setelah 10 menit), `rejected` (rollback / versi di .bin tidak cocok, tidak dicoba lagi).
- Batch offline (`CLOUD_BATCH_URL`): `{"data":[<telemetry>, ...]}`, tanpa `command_version`/`config`/`schedules`.
- Balasan server: `{"status":"success","device_command":{...}}` atau `"device_command": null`.
  - Berisi konfigurasi penuh jika `command_version` alat berbeda dari versi web: `version`, `system_mode`, `manual_mode_type`, `manual_duration`, `sensor_interval`, `log_interval`, `cooldown`, `schedules`.
  - Berisi `relays` (`[{"relay":"main"|1..3,"on":bool}]`) jika ada perintah relay dari web. Diterapkan terakhir dan hanya di mode Manual.
  - `applyDeviceCommand()` mencatat `version` setelah semua isi diterapkan. Setelah restart versinya 0, sehingga server mengirim ulang konfigurasi web.
- Tawaran OTA ada di key level atas `ota` (bukan di dalam `device_command`): `{"version","path","size","md5"}`, atau `null`. Alat mengunduh `CLOUD_BASE_URL + path` dengan header `X-Device-Token`, lalu mencocokkan ukuran dan MD5-nya.
