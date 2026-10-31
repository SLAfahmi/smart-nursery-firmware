# Rencana OTA Cloud — Server Laravel

> **Status:** rencana, belum diimplementasikan.
> Repo target: `C:\HOSTING_SMART_NURSERY_WEB` (Laravel 13 / PHP 8.3).
> Pasangannya untuk sisi firmware ada di `OTA_PLAN_FIRMWARE.md`, di folder `docs/` repo firmware.

## Konteks

ESP32 Smart Nursery akan mendukung OTA cloud. Admin mengupload `firmware.bin` lewat dashboard web,
lalu alat mengambilnya sendiri melalui balasan telemetry yang dikirim setiap 5 detik. Dashboard lokal
alat (`web/` di repo firmware) sudah tertanam di dalam `firmware.bin`. Jadi server hanya perlu
menyimpan dan menyajikan **satu file .bin** untuk setiap rilis.

Sisi server bertugas:
1. menyimpan rilis firmware (upload dari tab Config),
2. menawarkan rilis aktif ke alat lewat key `ota` di balasan `POST /api/v1/telemetry`,
3. menyajikan file .bin ke alat (dilindungi `X-Device-Token`),
4. menampilkan versi firmware di alat dan status OTA-nya di dashboard.

Di firmware, update hanya dijalankan saat semua relay mati. Firmware yang gagal terhubung ke server
setelah update akan rollback sendiri dan melaporkan status `rejected`.

---

## Kontrak API (sama dengan dokumen firmware)

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
  - `failed`: gagal, alat mencoba lagi setelah 10 menit.
  - `rejected`: rollback, atau versi di file tidak cocok. Alat tidak akan mencoba versi itu lagi.
- Firmware lama yang belum mendukung OTA tidak mengirim `firmware_version`.

**Balasan telemetry (server → alat)** mendapat key level atas `ota`, di samping `device_command`. Jangan
masukkan key ini ke dalam `device_command`, karena `device_command` yang tidak null membuat alat mengirim
telemetry konfirmasi cepat.
```json
{ "status": "success", "device_command": null,
  "ota": { "version": "1.0.2", "path": "/api/v1/firmware/7/download", "size": 1071520, "md5": "<32 hex>" } }
```
- `path` harus **relatif**. Alat menyusun URL-nya dari `CLOUD_BASE_URL + path`, sehingga tidak terjadi
  salah skema http/https di belakang proxy hosting, dan mode server lokal tetap berfungsi.
- Server mengirim `ota: null` dalam kondisi berikut:
  - tidak ada rilis aktif,
  - versi alat sudah sama dengan rilis,
  - alat tidak mengirim `firmware_version`, atau
  - alat melaporkan `rejected` untuk versi rilis itu.

**Download**: `GET /api/v1/firmware/{id}/download` dengan header `X-Device-Token`. Balasannya
`application/octet-stream` dengan `Content-Length` dan header `x-MD5`.

---

## File yang diubah atau dibuat

| File | Perubahan |
|---|---|
| `database/migrations/2026_10_29_000001_add_firmware_ota.php` (baru) | Tabel `firmware_releases` + kolom `device_states.firmware_version`, `ota_status` |
| `app/Models/FirmwareRelease.php` (baru) | `active()`, `toOtaOffer()` |
| `app/Models/DeviceState.php` | Tambah `firmware_version` dan `ota_status` ke fillable/casts (`ota_status` => `array`) |
| `app/Http/Controllers/Api/TelemetryController.php` | `firmwareState()`, `otaOffer()`, simpan ke DeviceState, kirim `ota` di balasan `store()` |
| `app/Http/Controllers/FirmwareController.php` (baru) | `download` (API alat), `upload`, `cancel` (web) |
| `routes/api.php`, `routes/web.php` | Route download (token) dan upload/cancel (auth) |
| `app/Http/Controllers/DashboardController.php` | `getStatus()` mendapat blok `firmware` |
| `resources/views/dashboard.blade.php` | Kartu "Update Firmware (OTA)" di tab Config + JS. Komentar `data/www/index.html` diubah menjadi `web/index.html` |
| `tests/Feature/FirmwareOtaTest.php` (baru) | Test kontrak |

---

## Contoh kode

### Migration `2026_10_29_000001_add_firmware_ota.php`
```php
public function up(): void
{
    Schema::create('firmware_releases', function (Blueprint $table) {
        $table->id();
        $table->string('version', 32)->unique();
        $table->string('path');                       // relatif ke disk 'local'
        $table->unsignedInteger('size');
        $table->char('md5', 32);
        $table->text('notes')->nullable();
        $table->boolean('is_active')->default(false)->index();
        $table->foreignId('uploaded_by_user_id')->nullable()->constrained('users')->nullOnDelete();
        $table->timestamps();
    });

    Schema::table('device_states', function (Blueprint $table) {
        $table->string('firmware_version', 32)->nullable()->after('reported_version');
        $table->json('ota_status')->nullable()->after('firmware_version')
            ->comment('{"target","state","error"} dari alat');
    });
}

public function down(): void
{
    Schema::table('device_states', function (Blueprint $table) {
        $table->dropColumn(['firmware_version', 'ota_status']);
    });
    Schema::dropIfExists('firmware_releases');
}
```

### Model `FirmwareRelease`
```php
class FirmwareRelease extends Model
{
    protected $fillable = ['version', 'path', 'size', 'md5', 'notes', 'is_active', 'uploaded_by_user_id'];
    protected $casts = ['is_active' => 'boolean', 'size' => 'integer'];

    public static function active(): ?self
    {
        return static::where('is_active', true)->latest('id')->first();
    }

    /** Isi key "ota" pada balasan telemetry (lihat firmware src/ota.cpp: otaOffer) */
    public function toOtaOffer(): array
    {
        return [
            'version' => $this->version,
            'path' => "/api/v1/firmware/{$this->id}/download",
            'size' => $this->size,
            'md5' => $this->md5,
        ];
    }
}
```

### `TelemetryController`
Validator untuk field firmware dibuat terpisah dari `deviceState()`. Validator `deviceState()` mengembalikan
`[]` jika ada satu field yang tidak valid, sehingga satu field `ota` yang salah akan ikut membuang
sinkronisasi config.
```php
private function firmwareState(Request $request): array
{
    $validator = Validator::make($request->only(['firmware_version', 'ota']), [
        'firmware_version' => 'nullable|string|max:32',
        'ota' => 'nullable|array',
        'ota.target' => 'nullable|string|max:32',
        'ota.state' => 'nullable|string|in:deferred,ready,failed,rejected',
        'ota.error' => 'nullable|string|max:120',
    ]);

    if ($validator->fails()) {
        Log::warning('Status firmware dari alat diabaikan', $validator->errors()->toArray());

        return [];
    }

    return $validator->validated();
}

private function otaOffer(array $firmware): ?array
{
    $release = FirmwareRelease::active();
    $current = $firmware['firmware_version'] ?? null;

    // Tanpa versi = firmware lama yang belum mendukung OTA
    if (!$release || $current === null || $current === $release->version) {
        return null;
    }

    // Alat sudah menolak versi ini (rollback / versi di file tidak cocok)
    $ota = $firmware['ota'] ?? [];
    if (($ota['target'] ?? null) === $release->version && ($ota['state'] ?? null) === 'rejected') {
        return null;
    }

    return $release->toOtaOffer();
}
```

Perubahan di `store()`:
- Panggil `$firmware = $this->firmwareState($request);` di samping `$deviceState`.
- Teruskan `$firmware` ke `syncDevice()`. Di `DeviceState::updateOrCreate`, tambahkan:
  ```php
  'firmware_version' => $firmware['firmware_version'] ?? null,
  'ota_status' => $firmware['ota'] ?? null,
  ```
- Tambahkan `'ota' => $this->otaOffer($firmware),` ke balasan JSON, di samping `device_command`.

### `FirmwareController`
```php
class FirmwareController extends Controller
{
    /** API alat (middleware VerifyDeviceToken). Firmware memverifikasi MD5 dari tawaran telemetry. */
    public function download(FirmwareRelease $release): BinaryFileResponse
    {
        return response()->download(
            Storage::disk('local')->path($release->path),
            "smart-nursery-{$release->version}.bin",
            ['Content-Type' => 'application/octet-stream', 'x-MD5' => $release->md5]
        );
    }

    /** Web (auth): rilis firmware baru, rilis lama otomatis nonaktif */
    public function upload(Request $request): JsonResponse
    {
        $validated = $request->validate([
            // Harus sama persis dengan FIRMWARE_VERSION di include/config.h firmware
            'version'  => ['required', 'string', 'max:32', 'regex:/^\d+\.\d+\.\d+$/', 'unique:firmware_releases,version'],
            'firmware' => 'required|file|extensions:bin|max:6400',   // KB, partisi app 6.25 MB
            'notes'    => 'nullable|string|max:500',
        ]);

        $file = $request->file('firmware');
        // Image aplikasi ESP32 selalu diawali magic byte 0xE9
        if (file_get_contents($file->getRealPath(), false, null, 0, 1) !== "\xE9") {
            return response()->json(['status' => 'error', 'message' => 'File bukan firmware ESP32 (.bin)'], 422);
        }

        $user = Auth::user();
        $release = DB::transaction(function () use ($validated, $file, $user) {
            FirmwareRelease::where('is_active', true)->update(['is_active' => false]);

            return FirmwareRelease::create([
                'version' => $validated['version'],
                'path' => $file->storeAs('firmware', "{$validated['version']}.bin", 'local'),
                'size' => $file->getSize(),
                'md5' => md5_file($file->getRealPath()),
                'notes' => $validated['notes'] ?? null,
                'is_active' => true,
                'uploaded_by_user_id' => $user->id,
            ]);
        });

        ActivityLog::create([
            'user_id' => $user->id,
            'action' => "Merilis firmware {$release->version} (OTA)",
            'details_json' => ['size' => $release->size, 'md5' => $release->md5],
        ]);

        return response()->json(['status' => 'success', 'message' => "Firmware {$release->version} dirilis oleh {$user->name}"]);
    }

    /** Web (auth): hentikan penawaran rilis. Alat yang sudah di status "ready" tetap restart ke versi itu. */
    public function cancel(FirmwareRelease $release): JsonResponse
    {
        $release->update(['is_active' => false]);

        ActivityLog::create([
            'user_id' => Auth::id(),
            'action' => "Membatalkan rilis firmware {$release->version}",
            'details_json' => ['release_id' => $release->id],
        ]);

        return response()->json(['status' => 'success']);
    }
}
```

### Route
```php
// routes/api.php — di dalam grup v1 (throttle:60,1 + VerifyDeviceToken)
Route::get('/firmware/{release}/download', [FirmwareController::class, 'download']);

// routes/web.php — di dalam grup auth
Route::post('/api/firmware', [FirmwareController::class, 'upload'])->name('firmware.upload');
Route::post('/api/firmware/{release}/cancel', [FirmwareController::class, 'cancel'])->name('firmware.cancel');
```

### Dashboard
- Tambahkan blok berikut ke `DashboardController::getStatus()`:
  ```php
  'firmware' => [
      'device_version' => $deviceState?->firmware_version,
      'ota' => $deviceState?->ota_status,
      'release' => ($release = FirmwareRelease::active()) ? [
          'id' => $release->id,
          'version' => $release->version,
          'size' => $release->size,
          'uploaded_at' => $release->created_at->format('d/m/Y H:i'),
      ] : null,
  ],
  ```
- **Blade, tab Config**: tambahkan kartu "Update Firmware (OTA)" dengan pola `config-item` yang sama
  seperti kartu "Sinkronisasi Waktu RTC". Isinya:
  - versi di alat dan rilis aktif,
  - chip status,
  - form: input versi, file `.bin` (`accept=".bin"`), dan catatan,
  - tombol **Rilis** dan **Batalkan**.
- **JS**:
  - `uploadFirmware(btn)` mengirim `FormData` dengan header `X-CSRF-TOKEN`. Jangan pakai `postJson`,
    karena request ini multipart.
  - `renderFirmware(data.firmware)` dipanggil dalam polling, di samping `renderClock()`.
  - Isi chip status:

  | Kondisi | Chip |
  |---|---|
  | Tidak ada rilis aktif | "Tidak ada rilis" |
  | `device_version == release.version` | "Terpasang" (ok) |
  | `ota.target == release.version` dan `state = deferred` | "Menunggu relay mati" |
  | `… state = ready` | "Menunggu restart" |
  | `… state = failed` | "Gagal: {error} (dicoba lagi)" (bad) |
  | `… state = rejected` | "Ditolak alat / rollback" (bad) |
  | `device_version` null | "Firmware alat belum mendukung OTA (upload USB sekali)" |
  | selain itu | "Menunggu alat mengambil" |

---

## Test `tests/Feature/FirmwareOtaTest.php`

Ikuti gaya `TelemetryApiTest`. Pakai `Storage::fake('local')` dan
`UploadedFile::fake()->createWithContent('fw.bin', "\xE9" . str_repeat("\0", 1023))`.

| Kasus | Hasil yang diharapkan |
|---|---|
| Telemetry `firmware_version` lama + rilis aktif | `ota.version`, `ota.path`, `ota.size`, `ota.md5` sesuai rilis |
| Versi sama dengan rilis | `ota` null |
| Tanpa `firmware_version` | `ota` null |
| `ota.state = rejected` untuk versi rilis | `ota` null |
| `ota` tidak valid (mis. state asing) | Telemetry tetap 201, config sync tidak terganggu |
| Telemetry menyimpan `firmware_version`/`ota_status` | Kolom di `device_states` terisi |
| Download tanpa token | 401 |
| Download dengan token | 200, header `x-MD5`, isi file sama |
| Upload file yang byte pertamanya bukan 0xE9 | 422 |
| Upload versi duplikat / format salah | 422 |
| Upload valid | Rilis baru aktif, rilis lama nonaktif, ActivityLog tercatat |
| Cancel | `is_active = false`, telemetry berikutnya mendapat `ota` null |

---

## Catatan hosting

- `upload_max_filesize` dan `post_max_size` PHP sebaiknya ≥ 8M. Default 2M masih cukup untuk firmware
  saat ini (±1.05 MB), tetapi ruangnya sempit.
- Pastikan web server tidak mem-gzip atau men-chunk `application/octet-stream`, karena alat membutuhkan
  `Content-Length`. `BinaryFileResponse` sudah mengirim header itu.
- File firmware tersimpan di `storage/app/private/firmware/`, yaitu disk `local` di Laravel 11+, sehingga
  tidak bisa diakses publik.

## Verifikasi

1. Jalankan `php artisan migrate`, lalu `php artisan test`. Seluruh test harus lolos, termasuk
   `FirmwareOtaTest`.
2. Untuk uji end-to-end bersama alat, jalankan `php artisan serve --host=0.0.0.0` dengan
   `USE_LOCAL_SERVER` aktif di firmware. Langkah 2–6 di bagian Verifikasi dokumen firmware mencakup OTA
   sukses, keamanan relay, MD5, rollback, dan versi tidak cocok.
