# Revisi Keamanan — Server Laravel

> **Status:** ringkasan + rencana, belum diimplementasikan.
> Repo target: `C:\HOSTING_SMART_NURSERY_WEB` (Laravel 13 / PHP 8.3).
> Pasangan sisi firmware sudah dikerjakan di repo firmware (`include/secrets.h`, login dashboard lokal).

## Konteks

Revisi keamanan firmware:
- Kredensial (`DEVICE_KEY`, password Wi-Fi) pindah dari `include/config.h` ke `include/secrets.h`, yang tidak ikut ter-commit.
- Dashboard lokal alat sekarang memakai login (Digest Auth) dan proteksi CSRF.

**Kontrak API alat ↔ server tidak berubah**, jadi server tetap berjalan tanpa perubahan apa pun. Namun nilai
`DEVICE_KEY` lama sudah tersimpan di riwayat commit GitHub firmware. Nilai itu sama dengan `DEVICE_TOKEN` di
`.env` server, sehingga **token harus diganti**. Hasil pengecekan kode server juga menemukan beberapa celah
kecil yang dirangkum di bawah.

---

## Yang sudah aman (tidak perlu diubah)

| Aspek | Implementasi sekarang |
|---|---|
| Cek token alat | `VerifyDeviceToken` memakai `hash_equals` dan menolak jika `DEVICE_TOKEN` kosong |
| Rate limit API alat | `throttle:60,1` di grup `/api/v1` (alat hanya ±12 request/menit) |
| Login web | `throttle:10,1`, register mati (`ALLOW_REGISTRATION=false`), akun dibuat lewat `nursery:user` |
| Header keamanan | `SecurityHeaders`: `X-Frame-Options`, `nosniff`, `Referrer-Policy`, HSTS di HTTPS |
| File firmware OTA | Disk privat, cek magic byte `0xE9`, MD5 dihitung server, download wajib token |
| Validasi config/jadwal/OTA | Rentang nilai sudah dibatasi (`deviceState()`, `firmwareState()`) |
| `.env` | Ada di `.gitignore` dan tidak pernah ter-commit |

---

## Yang perlu dilakukan

### 1. Ganti `DEVICE_TOKEN` (wajib, segera)
- Buat token baru yang panjang dan acak (token lama hanya 11 karakter):
  `php -r "echo bin2hex(random_bytes(24)), PHP_EOL;"` → 48 karakter hex.
- Isi nilai yang sama di dua tempat: `DEVICE_TOKEN` di `.env` hosting dan `DEVICE_KEY` di `include/secrets.h` firmware.
- **Urutan yang aman:** ganti `.env` hosting, jalankan `php artisan config:cache` (jika config di-cache, perubahan
  `.env` tidak terbaca sebelum perintah ini), lalu segera upload firmware **lewat USB**. Selama jeda, alat
  menerima 401 dan menyimpan data ke buffer offline (maks 50 snapshot, tiap 30 detik ≈ 25 menit). Data itu
  terkirim otomatis setelah token cocok.
- **Jangan mengganti token lewat OTA.** Firmware baru dengan token baru akan ditolak server lama, sehingga tidak
  pernah terkonfirmasi. Jika alat restart sebelum server ikut diganti, bootloader rollback ke firmware lama.
- Perbarui komentar yang masih menyebut `include/config.h` menjadi `include/secrets.h`:
  `app/Http/Middleware/VerifyDeviceToken.php`, `config/nursery.php`, `.env.example`.

### 2. `.env` produksi di hosting
`.env` lokal saat ini `APP_ENV=local` dan `APP_DEBUG=true`. Itu wajar untuk laptop, tetapi di hosting harus:
```
APP_ENV=production
APP_DEBUG=false          # debug=true menampilkan stack trace & isi konfigurasi ke publik
APP_URL=https://smartnursery-tel-u.srs-ssms.com
SESSION_SECURE_COOKIE=true
```
Jalankan `php artisan config:cache` setelah mengubahnya.

### 3. Batasi nilai sensor di telemetry
`telemetryRules()` hanya mengecek `numeric`, jadi nilai seperti suhu `99999` tetap tersimpan. Rentang nilai
**jangan** dijadikan rule validasi yang membalas 422. Jika satu sensor rusak lalu mengirim nilai aneh, seluruh
telemetry ditolak: alat menganggap server mati dan `device_command` tidak pernah sampai. Lebih aman nilai di
luar rentang **diubah menjadi `null` lalu dicatat ke log**, sedangkan struktur (`zones`, nomor zona, tipe data)
tetap divalidasi ketat seperti sekarang.

| Field | Rentang | Sumber di firmware |
|---|---|---|
| `temperature` | -40 … 80 | rentang DHT |
| `humidity` | 0 … 100 | % |
| `light` | 0 … 65535 | BH1750 |
| `zones.*.soil` | 0 … 100 | sudah di-clamp firmware |
| `zones.*.fuzzy_duration` | 0 … 3600 | detik |
| `zones.*.state` | `idle`, `queued`, `watering`, `soaking` | `toString(ZoneState)` |
| `recorded_at` | ≥ 2024-01-01 dan ≤ sekarang + 1 hari | firmware menolak epoch < 2024-01-01 |

```php
// TelemetryController — dipanggil setelah validate(), untuk store() dan setiap item syncBatch()
private function sanitizeTelemetry(array $data): array
{
    $clamp = function (string $field, $value, float $min, float $max) {
        if ($value === null || ($value >= $min && $value <= $max)) {
            return $value;
        }
        Log::warning("Telemetry {$field} di luar rentang, disimpan null", ['value' => $value]);
        return null;
    };

    $data['temperature'] = $clamp('temperature', $data['temperature'] ?? null, -40, 80);
    $data['humidity']    = $clamp('humidity', $data['humidity'] ?? null, 0, 100);
    $data['light']       = $clamp('light', $data['light'] ?? null, 0, 65535);

    foreach ($data['zones'] as $i => $zone) {
        $data['zones'][$i]['soil'] = $clamp('soil', $zone['soil'] ?? null, 0, 100);
        $data['zones'][$i]['fuzzy_duration'] = $clamp('fuzzy_duration', $zone['fuzzy_duration'] ?? null, 0, 3600);
        if (!in_array($zone['state'] ?? null, [null, 'idle', 'queued', 'watering', 'soaking'], true)) {
            $data['zones'][$i]['state'] = null;
        }
    }

    // Jam alat tidak masuk akal: perlakukan sama seperti jam belum valid
    if (!empty($data['recorded_at'])) {
        $t = Carbon::parse($data['recorded_at']);
        if ($t->lt('2024-01-01') || $t->gt(now()->addDay())) {
            $data['recorded_at'] = null;
        }
    }

    return $data;
}
```

Batas batch di `syncBatch()` juga diturunkan: `'data' => 'required|array|min:1|max:60'` (sebelumnya `max:500`).
Firmware tidak pernah mengirim lebih dari 50 (`MAX_OFFLINE_ENTRIES`), dan batas 500 memperbesar beban jika token
bocor dipakai untuk membanjiri data.

### 4. Catat token yang ditolak
Tambahkan log di `VerifyDeviceToken` sebelum membalas 401, supaya percobaan memakai token lama bisa terlihat.
**Jangan pernah mencatat isi token.**
```php
Log::warning('Device token ditolak', ['ip' => $request->ip(), 'path' => $request->path()]);
```

### 5. Pastikan HTTPS benar-benar terdeteksi di hosting
HSTS hanya dikirim jika `$request->isSecure()` bernilai true. Jika hosting memakai reverse proxy/CDN, Laravel
melihat request sebagai HTTP sehingga HSTS tidak pernah terkirim.
- Cek: `curl -I https://smartnursery-tel-u.srs-ssms.com/login` → harus ada header `Strict-Transport-Security`.
- Jika tidak ada, tambahkan `$middleware->trustProxies(at: '*');` di `bootstrap/app.php`. Tambahkan hanya jika
  memang ada proxy di depan aplikasi.

### 6. Hak akses admin (opsional)
Saat ini setiap akun yang login bisa menyalakan pompa, mereset log, dan **merilis firmware**. Jika akun lebih
dari satu orang, tambahkan kolom `users.is_admin` dan `Gate::define('admin', ...)`. Lindungi
`firmware.upload`, `firmware.cancel`, dan `dashboard.reset-log` dengan `->middleware('can:admin')`.

---

## Lanjutan (butuh perubahan firmware juga, belum dikerjakan)

| Revisi | Inti | Catatan |
|---|---|---|
| Tanda tangan HMAC | Alat mengirim `X-Timestamp` dan `X-Signature = HMAC-SHA256(token, timestamp + "." + body)`, bukan token polos. Server menolak jika selisih waktu > 5 menit atau tanda tangan salah | Mencegah replay dan pemalsuan isi. Butuh jam alat valid (NTP/RTC) |
| Tanda tangan firmware | Saat upload, `.bin` ditandatangani ECDSA P-256 dengan private key admin. Alat memverifikasi dengan public key yang tertanam di firmware | Firmware jahat tidak bisa dipasang walaupun server dibobol |

---

## File yang diubah

| File | Perubahan |
|---|---|
| `.env` (hosting) | Token baru, `APP_ENV`, `APP_DEBUG`, `SESSION_SECURE_COOKIE` |
| `app/Http/Controllers/Api/TelemetryController.php` | `sanitizeTelemetry()`, batas batch `max:60` |
| `app/Http/Middleware/VerifyDeviceToken.php` | Log penolakan, komentar `secrets.h` |
| `config/nursery.php`, `.env.example` | Komentar `secrets.h` |
| `bootstrap/app.php` | `trustProxies` (hanya jika perlu, lihat poin 5) |
| `tests/Feature/TelemetryApiTest.php` | Kasus baru di bawah |

## Test tambahan

| Kasus | Hasil yang diharapkan |
|---|---|
| Token lama / salah | 401, log `Device token ditolak` tanpa isi token |
| `humidity: 150` | 201, tersimpan `null` |
| `zones.*.state` asing | 201, state `null` |
| `recorded_at` tahun 2000 / tahun depan | 201, diperlakukan seperti `null` |
| Batch berisi 61 item | 422 |
| Batch berisi 50 item valid | 200, semua tersimpan |

## Verifikasi
1. `php artisan test`: semua test lolos.
2. Setelah token diganti di kedua sisi, Serial Monitor alat menampilkan telemetry `HTTP 200` dan buffer offline
   terkirim (`[CLOUD] Buffer offline terkirim`).
3. `curl -X POST -H "X-Device-Token: <token lama>" https://smartnursery-tel-u.srs-ssms.com/api/v1/telemetry` → 401.
4. `curl -I https://smartnursery-tel-u.srs-ssms.com/login` → ada `Strict-Transport-Security`, dan halaman error
   tidak menampilkan stack trace.
