# Smart Nursery Firmware (ESP32-S3 N16R8)

Irigasi otomatis dengan 3 mode: **Auto (Fuzzy Mamdani)**, **Manual**, dan **Schedule**.
Dashboard lokal tersedia lewat WiFi AP, dan data dikirim ke server Laravel lewat WiFi STA.

## Cara upload

1. Isi kredensial di `include/config.h`: `STA_PASSWORD`, `CLOUD_URL`, `CLOUD_BATCH_URL`, `DEVICE_KEY`.
2. Upload firmware: `pio run -t upload`
3. Upload dashboard (folder `data/`): `pio run -t uploadfs`
4. Buka Serial Monitor: `pio device monitor`
5. Sambungkan HP ke WiFi `SmartNursery-TelU`, lalu buka `http://192.168.4.1`

> Langkah 3 wajib diulang setiap kali file di `data/www/` diubah.

## Struktur

```
include/config.h          semua pin, kredensial, konstanta & default
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
src/tasks/*.cpp           satu file per task FreeRTOS
data/www/                 dashboard: index.html, style.css, app.js
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
| CloudSyncTask | 0 | 2 | 12288 | telemetry, buffer offline, NTP, reconnect WiFi |
| async_tcp (library) | 1 | 10 | 16384 | melayani web server (pengganti WebTask) |

## Perbaikan web lambat & RTC

**Web lambat**
- Web server diganti ke `ESPAsyncWebServer` (event-driven), jadi tidak perlu task yang polling `handleClient()`.
- `WiFi.setSleep(false)`: modem sleep menambah latensi ratusan ms per request.
- Auto-reconnect STA dimatikan. Saat router tidak ada, reconnect bawaan terus scan kanal dan membuat AP tersendat. Reconnect sekarang dicoba tiap 20 detik oleh CloudSyncTask.
- TLS/HTTPS (CloudSyncTask) berjalan di Core 0, web server di Core 1.
- Dashboard: polling hanya mengirim request baru setelah request sebelumnya selesai, setiap request punya timeout, polling berhenti saat tab tidak aktif, dan `style.css`/`app.js` di-cache browser.
- Tidak ada I/O (I2C, file, jaringan) selama `lockState()` dipegang, jadi handler web tidak pernah menunggu lama.

**RTC tidak bisa di-sync**
- BH1750 dan DS3231 berbagi bus I2C tetapi sebelumnya diakses dari task berbeda tanpa pengaman, sehingga transaksinya bisa bertabrakan. Sekarang semua akses I2C memakai `lockI2C()`.
- Task membaca waktu dari jam internal ESP32, bukan dari RTC. RTC hanya dibaca saat boot dan ditulis saat sync, lalu hasil tulis diverifikasi dengan membacanya kembali.
- Sync dari browser mengirim epoch. Jika RTC gagal ditulis, jam sistem tetap diperbarui dan dashboard menampilkan alasannya.
- Jika internet tersedia, waktu diambil otomatis dari NTP lalu disimpan ke RTC.
- Status RTC (terdeteksi atau tidak) tampil di tab Config.

## REST API lokal

| Endpoint | Method | Fungsi |
|---|---|---|
| `/api/data` | GET | sensor, zona, relay, waktu, status WiFi/cloud |
| `/api/override?mode=0\|1\|2` | POST | ganti mode sistem |
| `/api/relay?relay=main\|1..3` | POST | toggle relay (mode Manual) |
| `/api/config` | GET/POST | `sensorInterval`, `manualMode`, `manualDuration`, `logInterval`, `cooldown` |
| `/api/schedule` | GET/POST | `[{"zone":1,"slots":[{"active":true,"hour":6,"minute":0,"duration":60}]}]` |
| `/api/log?limit=N` | GET | log historis (JSON) |
| `/api/log.csv` | GET | unduh log CSV |
| `/api/log/reset` | POST | hapus log |
| `/api/sync-time` | POST | `{"epoch": 1760000000}` |

## Format cloud

- Telemetry (`CLOUD_URL`): `{"recorded_at","temperature","humidity","light","system_mode","relay_main","zones":[{"zone","soil","relay","state","fuzzy_duration"}]}`
- Batch offline (`CLOUD_BATCH_URL`): `{"data":[<telemetry>, ...]}`
- Header: `X-Device-Token: DEVICE_KEY`
- Balasan server (opsional): `{"device_command":{"system_mode","manual_mode_type","manual_duration","schedules"}}`. Format `schedules` sama dengan `/api/schedule`.
