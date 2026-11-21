'use strict';
const el = id => document.getElementById(id);
let key = '', data = null, received = 0, marker = null, first = true;
let generation = 0, busy = false, apiOK = false;
const map = L.map('map').setView([22.5, 79], 4); // Overview only; never a simulated tracker position.
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
    maxZoom: 19, attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
}).on('tileerror', () => {el('map-note').textContent = 'Map tiles unavailable. Coordinates and status may still work.';}).addTo(map);
L.control.scale({imperial: false}).addTo(map);
map.on('dragstart', () => {el('follow').checked = false;});
function centre(reset = false) {
    if (!data?.position) return;
    const p = data.position, ll = L.latLng(p.lat, p.lon);
    if (!reset) {map.panTo(ll); return;}
    const mode = el('scale').value;
    if (mode === 'region') map.fitBounds(ll.toBounds(200000), {padding: [12, 12]});
    else map.setView(ll, mode === 'campus' ? 16 : 13);
}
el('recentre').onclick = () => centre(true);
el('scale').onchange = () => centre(true);
el('follow').onchange = () => {if (el('follow').checked) centre();};
function clearPosition() {
    if (marker) {map.removeLayer(marker); marker = null;}
    data = null; first = true;
    for (const id of ['coords','fix-time','age','contact','quality','motion','imu','reason']) el(id).textContent = '—';
}
el('login').onsubmit = e => {
    e.preventDefault(); generation++; key = el('token').value.trim(); el('token').value = '';
    clearPosition(); apiOK = false; el('status').textContent = 'Connecting…'; poll();
};
el('logout').onclick = () => {
    generation++; key = ''; apiOK = false; clearPosition(); el('status').textContent = 'Disconnected';
};
async function poll() {
    if (!key || busy) return;
    busy = true; const g = generation;
    try {
        const r = await fetch('/api/latest', {headers: {Authorization: 'Bearer ' + key}, cache: 'no-store', signal: AbortSignal.timeout(8000)});
        if (g !== generation) return;
        if (r.status === 401) {key = ''; clearPosition(); throw new Error('Incorrect viewer token');}
        if (!r.ok) throw new Error('Server unavailable');
        const next = await r.json(); if (g !== generation) return;
        data = next; received = performance.now(); apiOK = true;
        if (data.position) {
            const p = data.position, ll = [p.lat, p.lon];
            if (!marker) marker = L.circleMarker(ll, {radius: 9, color: '#17644c', weight: 3, fillOpacity: .85}).addTo(map);
            else marker.setLatLng(ll);
            el('coords').textContent = `${p.lat.toFixed(6)}, ${p.lon.toFixed(6)}`;
            el('fix-time').textContent = new Date(p.fix_at * 1000).toISOString();
            if (first) {centre(true); first = false;} else if (el('follow').checked) centre();
        } else {
            if (marker) {map.removeLayer(marker); marker = null;}
            first = true; el('coords').textContent = '—'; el('fix-time').textContent = '—';
        }
        renderStatus();
    } catch (err) {
        if (g === generation) {apiOK = false; el('status').textContent = err.message === 'Incorrect viewer token' ? err.message : 'Cannot reach server — location unconfirmed'; if (marker) marker.setStyle({color:'#7b8580'});}
    } finally {busy = false;}
}
function renderStatus() {
    if (!data) return;
    const now = data.server_now + (performance.now() - received) / 1000;
    const hb = data.heartbeat_at === null ? Infinity : Math.max(0, now - data.heartbeat_at);
    const age = data.position ? Math.max(0, now - data.position.fix_at) : Infinity;
    const t = data.telemetry;
    const live = apiOK && hb < 15 && age < 10 && t?.fix_ok;
    if (apiOK) el('status').textContent = hb === Infinity ? 'Waiting for device' : hb >= 15 ? 'Device offline / unreachable' : live ? 'Live position' : 'Device online — waiting for usable GPS';
    if (marker) marker.setStyle({color: live ? '#17644c' : '#7b8580'});
    el('age').textContent = Number.isFinite(age) ? `${Math.floor(age)} seconds` : 'No position yet';
    el('contact').textContent = Number.isFinite(hb) ? `${Math.floor(hb)} seconds ago` : 'Never';
    if (t) {
        el('quality').textContent = `${t.satellites ?? '—'} / ${t.hdop ?? '—'}`;
        el('motion').textContent = `${t.speed_kmph ?? '—'} km/h / ${t.altitude_m ?? '—'} m`;
        el('imu').textContent = t.mpu_ok ? 'Responding (MPU6050)' : 'Unavailable; GPS continues';
        el('reason').textContent = t.reason;
    }
}
setInterval(poll, 2000); setInterval(renderStatus, 1000);
