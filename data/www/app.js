// =============================================================
//  Smart Nursery Dashboard
//
//  Supaya dashboard tetap responsif walau ESP32 sibuk:
//   - Polling /api/data berantai: request berikutnya baru dikirim setelah
//     request sebelumnya selesai (tidak menumpuk seperti setInterval).
//   - Setiap request punya timeout, jadi UI tidak "menggantung".
//   - Polling berhenti saat tab browser tidak aktif.
//   - Aksi tombol langsung mengubah tampilan, lalu data di-refresh.
// =============================================================

const POLL_INTERVAL_MS = 3000;
const REQUEST_TIMEOUT_MS = 6000;
const MODE_LABELS = ['Auto (Fuzzy)', 'Manual', 'Schedule'];
const MAX_SLOTS = 3;

const $ = (id) => document.getElementById(id);

let renderedZoneCount = 0;
let pollTimer = null;
let pollInFlight = false;
let refreshQueued = false;

// ============ HTTP HELPER ============
async function api(path, options = {}) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
    try {
        const res = await fetch(path, { ...options, signal: controller.signal });
        const isJson = (res.headers.get('content-type') || '').includes('application/json');
        const body = isJson ? await res.json() : await res.text();
        if (!res.ok) throw new Error((body && body.error) || ('HTTP ' + res.status));
        return body;
    } catch (e) {
        if (e.name === 'AbortError') throw new Error('Perangkat tidak merespons (timeout)');
        throw e;
    } finally {
        clearTimeout(timer);
    }
}

function postJson(path, data) {
    return api(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
    });
}

// Nonaktifkan tombol selama request berjalan (cegah klik ganda)
async function withBusy(button, task) {
    if (button) button.disabled = true;
    try {
        await task();
    } catch (e) {
        toast(e.message, 'error');
    } finally {
        if (button) button.disabled = false;
    }
}

// ============ TOAST ============
let toastTimer = null;
function toast(message, type = 'ok') {
    const el = $('toast');
    el.textContent = message;
    el.className = 'toast show ' + type;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.className = 'toast'; }, type === 'error' ? 5000 : 3000);
}

// ============ FORMAT ============
function fmt(value, digits = 1) {
    return (value === null || value === undefined || Number.isNaN(value)) ? '--' : Number(value).toFixed(digits);
}

function formatDuration(sec) {
    if (sec >= 3600) return Math.floor(sec / 3600) + 'j ' + Math.floor((sec % 3600) / 60) + 'm';
    if (sec >= 60) return Math.floor(sec / 60) + 'm ' + (sec % 60) + 'd';
    return sec + 'd';
}

// ============ TAB & THEME ============
function switchTab(tabId) {
    ['dashboard', 'sensors', 'config'].forEach(t => {
        $('tab-' + t).style.display = (t === tabId) ? 'block' : 'none';
        $('nav-' + t).classList.toggle('active', t === tabId);
    });
    if (tabId === 'sensors') loadHistory();
    window.scrollTo(0, 0);
}

function applyTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    $('icon-sun').style.display = theme === 'dark' ? 'block' : 'none';
    $('icon-moon').style.display = theme === 'dark' ? 'none' : 'block';
}

function toggleTheme() {
    const next = document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
    applyTheme(next);
    try { localStorage.setItem('theme', next); } catch (e) { /* abaikan */ }
}

// ============ POLLING ============
async function poll() {
    pollInFlight = true;
    clearTimeout(pollTimer);
    try {
        render(await api('/api/data'));
        setConnection(true);
    } catch (e) {
        setConnection(false);
    } finally {
        pollInFlight = false;
        if (document.hidden) return;
        // Ada aksi user saat request berjalan -> ambil data terbaru segera
        const delay = refreshQueued ? 0 : POLL_INTERVAL_MS;
        refreshQueued = false;
        pollTimer = setTimeout(poll, delay);
    }
}

// Refresh segera (dipanggil setelah aksi user)
function refreshNow() {
    if (pollInFlight) refreshQueued = true;
    else poll();
}

document.addEventListener('visibilitychange', () => {
    if (document.hidden) clearTimeout(pollTimer);
    else refreshNow();
});

function setConnection(online) {
    $('conn-badge').classList.toggle('offline', !online);
    $('conn-text').textContent = online ? 'Live' : 'Offline';
}

// ============ RENDER DATA ============
function render(d) {
    const s = d.sensors;
    $('temp').textContent = fmt(s.temperature);
    $('hum').textContent = fmt(s.humidity);
    $('light').textContent = fmt(s.lux, 0);

    const soilAvg = d.zones.reduce((sum, z) => sum + z.soil, 0) / d.zones.length;
    $('soil').textContent = fmt(soilAvg);

    const problems = [];
    if (!s.dhtOk) problems.push('DHT22 (suhu/kelembaban)');
    if (!s.luxOk) problems.push('BH1750 (cahaya)');
    $('sensor-alert').style.display = problems.length ? 'block' : 'none';
    $('sensor-alert').textContent = problems.length
        ? 'Sensor bermasalah: ' + problems.join(', ') + '. Mode Auto tidak akan menyiram sampai data valid.' : '';

    renderPumpStatus(d);
    renderModeControls(d.mode);
    renderZones(d);
    renderTime(d);
    renderNetwork(d);
}

function renderPumpStatus(d) {
    const watering = d.zones.filter(z => z.state === 'watering').length;
    const queued = d.zones.filter(z => z.state === 'queued').length;
    const badge = $('irrig-mode');
    let detail = '';

    if (d.mode === 1) {
        badge.textContent = 'MANUAL';
        badge.className = 'mode-badge manual';
        detail = d.manualType === 1 ? 'Timer ' + d.manualDuration + ' dtk' : 'Toggle';
    } else if (watering > 0 || queued > 0) {
        badge.textContent = 'AKTIF (' + watering + ')' + (queued ? ' | ANTRE (' + queued + ')' : '');
        badge.className = 'mode-badge active';
        const remaining = Math.max(...d.zones.map(z => z.remaining));
        detail = remaining > 0 ? 'Sisa: <strong>' + formatDuration(remaining) + '</strong>' : '';
    } else if (d.mode === 2) {
        badge.textContent = 'SCHEDULE';
        badge.className = 'mode-badge schedule';
        detail = 'Menunggu jadwal';
    } else {
        badge.textContent = 'IDLE';
        badge.className = 'mode-badge';
        detail = 'Pompa: <strong>' + (d.relayMain.on ? 'ON' : 'OFF') + '</strong>';
    }
    $('irrig-detail').innerHTML = detail;
}

function renderModeControls(mode) {
    const select = $('systemModeSelect');
    // Jangan timpa pilihan saat user sedang membuka dropdown
    if (document.activeElement !== select) select.value = String(mode);
    $('manualSettings').style.display = mode === 1 ? 'flex' : 'none';
    $('scheduleSettings').style.display = mode === 2 ? 'flex' : 'none';
}

function renderTime(d) {
    const t = d.time;
    const clock = t.valid ? t.text.slice(11, 16) : '--:--';
    $('hero-clock').textContent = clock;
    $('timestamp').textContent = t.valid ? t.text : new Date().toLocaleTimeString('id-ID') + ' (jam HP)';

    $('rtc-time').textContent = t.valid ? t.text : 'Belum di-set';
    const sourceLabels = { none: 'Belum ada', rtc: 'RTC DS3231', browser: 'Browser', ntp: 'Internet (NTP)' };
    $('rtc-source').textContent = sourceLabels[t.source] || t.source;
    const chip = $('rtc-chip');
    chip.textContent = t.rtcOk ? 'Terdeteksi' : 'Tidak terdeteksi';
    chip.className = 'status-chip ' + (t.rtcOk ? 'ok' : 'bad');
}

function renderNetwork(d) {
    const wifi = $('chip-wifi');
    wifi.textContent = d.wifi.sta ? 'Internet: OK (' + d.wifi.rssi + ' dBm)' : 'Internet: Putus';
    wifi.classList.toggle('bad', !d.wifi.sta);

    const cloud = $('chip-cloud');
    let text = d.cloud.ok ? 'Cloud: Sinkron' : 'Cloud: Gagal';
    if (d.cloud.queued > 0) text += ' · ' + d.cloud.queued + ' antre';
    cloud.textContent = text;
    cloud.classList.toggle('bad', !d.cloud.ok);
}

// ============ ZONA / VALVE ============
function valveCardHtml(id, name, desc) {
    return `
    <div class="valve-card" id="card-${id}">
        <div class="valve-card-info">
            <div class="valve-name-row">
                <span class="valve-name">${name}</span>
                <span class="valve-mode-badge" id="badge-${id}">Auto</span>
            </div>
            <div class="valve-desc">${desc}</div>
            <div class="valve-metrics" id="metrics-${id}"></div>
        </div>
        <div class="valve-card-toggle">
            <label class="toggle-wrap disabled" id="toggle-wrap-${id}">
                <input type="checkbox" class="toggle-input" id="toggle-${id}" onchange="toggleRelay('${id}')" disabled>
                <div class="toggle-track"></div>
            </label>
            <span class="valve-toggle-label">Valve</span>
        </div>
    </div>`;
}

function renderValveCards(zoneCount) {
    let html = valveCardHtml('main', 'Pompa Utama', 'Relay pompa air utama');
    for (let i = 1; i <= zoneCount; i++) html += valveCardHtml(String(i), 'Zona ' + i, 'Sub-blok ' + i);
    $('valve-container').innerHTML = html;
    $('zone-count').textContent = zoneCount + ' Zona';
    renderedZoneCount = zoneCount;
}

function renderZones(d) {
    if (renderedZoneCount !== d.zones.length) renderValveCards(d.zones.length);
    const isManual = d.mode === 1;

    // Pompa utama
    let mainMetrics = '';
    if (d.relayMain.remaining > 0) mainMetrics = metric('Sisa', formatDuration(d.relayMain.remaining));
    updateValve('main', {
        on: d.relayMain.on, isManual, state: 'idle', mode: d.mode, metrics: mainMetrics
    });

    // Zona
    d.zones.forEach(z => {
        let metrics = metric('Tanah', fmt(z.soil) + '%');
        if (d.mode === 0) metrics += metric('Fuzzy', fmt(z.fuzzy, 0) + 's');
        if (z.remaining > 0) metrics += metric('Sisa', formatDuration(z.remaining));
        if (d.mode === 0 && z.cooldownLeft > 0 && z.state === 'idle') metrics += metric('Cooldown', formatDuration(z.cooldownLeft));
        updateValve(String(z.id), { on: z.relay, isManual, state: z.state, mode: d.mode, metrics });
    });
}

function metric(label, value) {
    return `<span class="valve-metric">${label}: <span>${value}</span></span>`;
}

function updateValve(id, v) {
    const toggle = $('toggle-' + id);
    const card = $('card-' + id);
    const badge = $('badge-' + id);

    toggle.checked = v.on;
    toggle.disabled = !v.isManual;
    $('toggle-wrap-' + id).classList.toggle('disabled', !v.isManual);
    $('metrics-' + id).innerHTML = v.metrics;

    card.classList.remove('active-zone', 'manual-zone', 'queued-zone');
    let label = v.mode === 2 ? 'Jadwal' : 'Auto';
    let badgeClass = '';

    if (v.isManual) {
        label = 'Manual';
        badgeClass = 'manual';
        if (v.on) card.classList.add('manual-zone');
    } else if (v.state === 'queued') {
        label = 'Antre';
        badgeClass = 'queued';
        card.classList.add('queued-zone');
    } else if (v.state === 'watering' || (id === 'main' && v.on)) {
        label = 'Menyiram';
        badgeClass = 'active';
        card.classList.add('active-zone');
    } else if (v.state === 'soaking') {
        label = 'Meresap';
        badgeClass = 'active';
    }
    badge.textContent = label;
    badge.className = 'valve-mode-badge ' + badgeClass;
}

// ============ AKSI: MODE & RELAY ============
async function changeSystemMode() {
    const select = $('systemModeSelect');
    const mode = parseInt(select.value, 10);
    renderModeControls(mode);           // tampilkan langsung tanpa menunggu server
    select.blur();
    try {
        await api('/api/override?mode=' + mode, { method: 'POST' });
        toast('Mode diubah ke ' + MODE_LABELS[mode]);
    } catch (e) {
        toast('Gagal ganti mode: ' + e.message, 'error');
    }
    refreshNow();
}

async function toggleRelay(id) {
    const toggle = $('toggle-' + id);
    toggle.disabled = true;
    try {
        await api('/api/relay?relay=' + id, { method: 'POST' });
    } catch (e) {
        toggle.checked = !toggle.checked;   // kembalikan tampilan
        toast(e.message, 'error');
    }
    refreshNow();
}

// ============ KONFIGURASI ============
async function loadConfig() {
    try {
        const c = await api('/api/config');
        $('cfg-interval').value = c.sensorInterval;
        $('cfg-manual-mode').value = c.manualMode;
        $('cfg-manual-dur').value = c.manualDuration;
        setSelectValue('cfg-log-interval', c.logInterval);
        setSelectValue('cfg-cooldown', c.cooldown);
    } catch (e) {
        toast('Gagal memuat konfigurasi: ' + e.message, 'error');
    }
}

// Pilih nilai di <select>; tambahkan opsi jika nilainya tidak ada di daftar
function setSelectValue(id, value) {
    const select = $(id);
    if (![...select.options].some(o => o.value === String(value))) {
        select.add(new Option(formatDuration(value), value));
    }
    select.value = String(value);
}

function saveConfig(button) {
    return withBusy(button, async () => {
        await postJson('/api/config', {
            sensorInterval: parseInt($('cfg-interval').value, 10),
            logInterval: parseInt($('cfg-log-interval').value, 10),
            cooldown: parseInt($('cfg-cooldown').value, 10)
        });
        toast('Konfigurasi berhasil disimpan');
    });
}

function saveManualSettings(button) {
    return withBusy(button, async () => {
        await postJson('/api/config', {
            manualMode: parseInt($('cfg-manual-mode').value, 10),
            manualDuration: parseInt($('cfg-manual-dur').value, 10)
        });
        toast('Pengaturan manual disimpan');
        refreshNow();
    });
}

// ============ JADWAL ============
function renderScheduleForm(zones) {
    $('schedule-container').innerHTML = zones.map((zone, z) => {
        const slots = zone.slots.map((slot, i) => `
            <div class="schedule-slot" id="sch-row-${z}-${i}">
                <label class="toggle-wrap toggle-small">
                    <input type="checkbox" class="toggle-input" id="sch-active-${z}-${i}" ${slot.active ? 'checked' : ''}>
                    <div class="toggle-track"></div>
                </label>
                <input type="time" class="slot-input" id="sch-time-${z}-${i}"
                    value="${String(slot.hour).padStart(2, '0')}:${String(slot.minute).padStart(2, '0')}">
                <input type="number" class="slot-input slot-duration" id="sch-dur-${z}-${i}"
                    min="1" max="3600" value="${slot.duration}">
                <span class="slot-unit">dtk</span>
            </div>`).join('');

        // Jumlah slot yang ditampilkan = slot aktif terakhir (minimal 1)
        let visible = 1;
        zone.slots.forEach((slot, i) => { if (slot.active) visible = i + 1; });

        return `
            <div class="schedule-zone" data-slots="${zone.slots.length}">
                <div class="schedule-zone-header">
                    <div class="schedule-zone-title"><div class="zone-dot"></div>Zona ${zone.zone}</div>
                    <select class="slot-input" id="sch-count-${z}" onchange="updateScheduleCount(${z})">
                        ${[1, 2, 3].map(n => `<option value="${n}" ${n === visible ? 'selected' : ''}>${n}x Sehari</option>`).join('')}
                    </select>
                </div>
                <div class="schedule-slots">${slots}</div>
            </div>`;
    }).join('');
    zones.forEach((_, z) => updateScheduleCount(z));
}

function updateScheduleCount(z) {
    const count = parseInt($('sch-count-' + z).value, 10);
    for (let i = 0; i < MAX_SLOTS; i++) {
        const row = $('sch-row-' + z + '-' + i);
        if (row) row.style.display = i < count ? 'flex' : 'none';
    }
}

async function loadSchedule() {
    try {
        renderScheduleForm(await api('/api/schedule'));
    } catch (e) {
        $('schedule-container').innerHTML = '<div class="alert-error">Gagal memuat jadwal: ' + e.message + '</div>';
    }
}

function saveSchedule(button) {
    const zones = document.querySelectorAll('#schedule-container .schedule-zone');
    const payload = [...zones].map((zoneEl, z) => {
        const count = parseInt($('sch-count-' + z).value, 10);
        const slots = [];
        for (let i = 0; i < parseInt(zoneEl.dataset.slots, 10); i++) {
            const [hour, minute] = ($('sch-time-' + z + '-' + i).value || '00:00').split(':').map(Number);
            slots.push({
                active: i < count && $('sch-active-' + z + '-' + i).checked,
                hour, minute,
                duration: parseInt($('sch-dur-' + z + '-' + i).value, 10) || 60
            });
        }
        return { slots };
    });

    return withBusy(button, async () => {
        renderScheduleForm(await postJson('/api/schedule', payload));
        toast('Jadwal berhasil disimpan');
    });
}

// ============ RTC ============
function syncRTCTime(button) {
    return withBusy(button, async () => {
        const result = await postJson('/api/sync-time', { epoch: Math.floor(Date.now() / 1000) });
        toast(result.message, result.rtc ? 'ok' : 'warn');
        refreshNow();
    });
}

// ============ RIWAYAT LOG ============
async function loadHistory() {
    const body = $('history-body');
    try {
        const rows = await api('/api/log?limit=30');
        const zoneCount = rows.length ? rows[0].zones.length : renderedZoneCount;

        let head = '<tr><th>Waktu</th><th>Mode</th><th>Suhu</th><th>Udara</th><th>Cahaya</th>';
        for (let i = 1; i <= zoneCount; i++) head += '<th>Zona ' + i + '</th>';
        $('history-head').innerHTML = head + '</tr>';

        if (!rows.length) {
            body.innerHTML = '<tr><td colspan="' + (5 + zoneCount) + '" class="text-muted">Belum ada data log.</td></tr>';
            return;
        }

        const modeNames = ['Auto', 'Manual', 'Jadwal'];
        body.innerHTML = rows.reverse().map(r => {
            const zones = r.zones.map(z => z.duration > 0
                ? `<td>${fmt(z.before)}→${fmt(z.after)}%<br><small>${z.duration} dtk</small></td>`
                : `<td>${fmt(z.after)}%</td>`).join('');
            return `<tr><td class="nowrap">${r.time}</td><td>${modeNames[r.mode] || '-'}</td>
                <td>${fmt(r.temp)}°C</td><td>${fmt(r.hum)}%</td><td>${fmt(r.lux, 0)}</td>${zones}</tr>`;
        }).join('');
    } catch (e) {
        body.innerHTML = '<tr><td class="text-muted">Gagal memuat log: ' + e.message + '</td></tr>';
    }
}

function resetLogData(button) {
    if (!confirm('Hapus semua data log? Tindakan ini tidak dapat dibatalkan.')) return;
    return withBusy(button, async () => {
        await api('/api/log/reset', { method: 'POST' });
        toast('Data log berhasil dihapus');
        loadHistory();
    });
}

// ============ INIT ============
try { if (localStorage.getItem('theme') === 'dark') applyTheme('dark'); } catch (e) { /* abaikan */ }
poll();
loadConfig();
loadSchedule();
