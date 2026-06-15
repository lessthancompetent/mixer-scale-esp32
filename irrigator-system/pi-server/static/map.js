/* Effluent Irrigator — Leaflet map controller */

const API = '';  // same-origin

// ── Map init ──────────────────────────────────────────────────────────────────
const map = L.map('map', { zoomControl: true }).setView([-43.5, 172.6], 14);

// Satellite tile layer (ESRI World Imagery — free, no API key)
L.tileLayer(
  'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
  { attribution: 'Tiles © Esri', maxZoom: 20 }
).addTo(map);

// ── Layer groups ──────────────────────────────────────────────────────────────
const trackLayer   = L.layerGroup().addTo(map);
const spreadLayer  = L.layerGroup().addTo(map);
const alertLayer   = L.layerGroup().addTo(map);
const liveLayer    = L.layerGroup().addTo(map);

let liveMarker     = null;
let activeSessionId = null;
let spreadLoaded   = false;

// ── Colours ───────────────────────────────────────────────────────────────────
const TRACK_COLOR  = '#ffffff';
const SPREAD_COLOR = '#4caf50';
const ALERT_COLOR  = '#f44336';

// ── Live position polling (every 15s) ─────────────────────────────────────────
async function pollLive() {
  try {
    const res = await fetch(`${API}/api/live`);
    if (!res.ok) return;
    const pos = await res.json();
    if (!pos) return;

    const latlng = [pos.lat, pos.lon];
    const pumpOn = pos.pump_on === 1;
    const speed  = (pos.speed_cms / 100).toFixed(1);
    const batt   = pos.battery_pct;
    const ts     = pos.timestamp ? new Date(pos.timestamp + 'Z').toLocaleTimeString() : '—';

    // Update sidebar stats
    document.getElementById('stat-pump').textContent  = pumpOn ? 'ON' : 'OFF';
    document.getElementById('stat-pump').style.color  = pumpOn ? '#81c784' : '#e57373';
    document.getElementById('stat-speed').textContent = `${speed} m/s`;
    document.getElementById('stat-batt').textContent  = `${batt}%`;
    document.getElementById('stat-time').textContent  = ts;

    // Live badge
    const badge = document.getElementById('live-badge');
    badge.textContent = pumpOn ? 'Pump ON' : 'GPS OK';
    badge.className   = 'badge ' + (pumpOn ? 'badge-green' : 'badge-amber');

    // Update or create live marker (blue pulsing dot)
    if (!liveMarker) {
      liveMarker = L.circleMarker(latlng, {
        radius: 8, color: '#1e88e5', fillColor: '#42a5f5',
        fillOpacity: 0.9, weight: 2
      }).bindTooltip('Irrigator', { permanent: false }).addTo(liveLayer);
      map.setView(latlng, map.getZoom() < 16 ? 16 : map.getZoom());
    } else {
      liveMarker.setLatLng(latlng);
    }
  } catch (e) { /* network hiccup */ }
}
setInterval(pollLive, 15000);
pollLive();

// ── Session list ──────────────────────────────────────────────────────────────
async function loadSessions() {
  const res  = await fetch(`${API}/api/sessions`);
  const list = await res.json();
  const sel  = document.getElementById('session-select');
  sel.innerHTML = '<option value="">— select session —</option>';
  list.forEach(s => {
    const opt   = document.createElement('option');
    opt.value   = s.id;
    const date  = s.start_time ? s.start_time.slice(0, 10) : '';
    const label = s.name || `Session ${s.id}`;
    opt.text    = `${label} (${date}) · ${s.point_count ?? 0} pts`;
    if (s.end_time === null) opt.text = '▶ ' + opt.text; // active
    sel.appendChild(opt);
  });
}
loadSessions();

document.getElementById('session-select').addEventListener('change', e => {
  const id = parseInt(e.target.value);
  if (!id) {
    clearSessionLayers();
    activeSessionId = null;
    document.getElementById('spread-card').classList.add('hidden');
    return;
  }
  activeSessionId = id;
  loadSessionData(id);
});

async function loadSessionData(sid) {
  clearSessionLayers();
  spreadLoaded = false;
  document.getElementById('spread-card').classList.remove('hidden');

  // Load track
  const posRes  = await fetch(`${API}/api/sessions/${sid}/positions`);
  const posList = await posRes.json();
  renderTrack(posList);

  // Load spread polygon
  const spRes = await fetch(`${API}/api/sessions/${sid}/spread`);
  if (spRes.ok) {
    const data = await spRes.json();
    if (data.geojson) {
      renderSpread(data.geojson, data.stats);
      spreadLoaded = true;
    } else if (data.error) {
      showSpreadStats(null);
    }
  }
}

// Reload spread while session is active (new positions may have arrived)
setInterval(() => {
  if (activeSessionId) loadSessionData(activeSessionId);
}, 60000);

// ── Track rendering ───────────────────────────────────────────────────────────
function renderTrack(posList) {
  trackLayer.clearLayers();
  if (posList.length < 2) return;

  const latlngs = posList.map(p => [p.lat, p.lon]);
  const poly    = L.polyline(latlngs, {
    color: TRACK_COLOR, weight: 2, opacity: 0.7,
    dashArray: '6 4'
  }).addTo(trackLayer);

  // Direction arrows every N points
  const step = Math.max(1, Math.floor(posList.length / 20));
  for (let i = step; i < posList.length; i += step) {
    const from = posList[i - step];
    const to   = posList[i];
    addArrow([from.lat, from.lon], [to.lat, to.lon]);
  }

  map.fitBounds(poly.getBounds(), { padding: [40, 40] });
}

function addArrow(from, to) {
  const angle = bearingDeg(from, to);
  const mid   = [(from[0] + to[0]) / 2, (from[1] + to[1]) / 2];
  const icon  = L.divIcon({
    html: `<div style="transform:rotate(${angle}deg);color:#fff;font-size:14px;line-height:1">▲</div>`,
    iconSize: [14, 14], className: ''
  });
  L.marker(mid, { icon, interactive: false }).addTo(trackLayer);
}

function bearingDeg(from, to) {
  const dLon = (to[1] - from[1]) * Math.PI / 180;
  const lat1 = from[0] * Math.PI / 180;
  const lat2 = to[0]   * Math.PI / 180;
  const y    = Math.sin(dLon) * Math.cos(lat2);
  const x    = Math.cos(lat1) * Math.sin(lat2) - Math.sin(lat1) * Math.cos(lat2) * Math.cos(dLon);
  return ((Math.atan2(y, x) * 180 / Math.PI) + 360) % 360;
}

// ── Spread polygon ────────────────────────────────────────────────────────────
function renderSpread(geojson, stats) {
  spreadLayer.clearLayers();
  L.geoJSON(geojson, {
    filter: f => f.properties.type === 'spread',
    style: {
      color:       '#2e7d32',
      fillColor:   SPREAD_COLOR,
      fillOpacity: 0.35,
      weight:      1.5
    }
  }).bindTooltip(f => {
    const p = f.properties;
    return `Area: ${(p.area_ha).toFixed(2)} ha<br>` +
           `Volume: ${(p.volume_l / 1000).toFixed(1)} m³<br>` +
           `Rate: ${p.app_rate_l_m2.toFixed(2)} L/m²<br>` +
           `Depth: ${p.depth_mm} mm`;
  }).addTo(spreadLayer);

  showSpreadStats(stats);
}

function showSpreadStats(stats) {
  if (!stats) {
    ['area', 'vol', 'rate', 'depth', 'dur', 'pts'].forEach(k =>
      (document.getElementById('stat-' + k).textContent = '—'));
    return;
  }
  document.getElementById('stat-area').textContent  = `${stats.area_ha} ha (${stats.area_m2.toLocaleString()} m²)`;
  document.getElementById('stat-vol').textContent   = `${(stats.volume_l / 1000).toFixed(1)} m³`;
  document.getElementById('stat-rate').textContent  = `${stats.app_rate_l_m2} L/m²`;
  document.getElementById('stat-depth').textContent = `${stats.depth_mm} mm`;
  document.getElementById('stat-dur').textContent   = `${stats.duration_min} min`;
  document.getElementById('stat-pts').textContent   = stats.point_count;
}

function clearSessionLayers() {
  trackLayer.clearLayers();
  spreadLayer.clearLayers();
}

// ── Alerts ────────────────────────────────────────────────────────────────────
async function pollAlerts() {
  try {
    const res    = await fetch(`${API}/api/alerts`);
    const alerts = await res.json();
    renderAlerts(alerts);
  } catch (e) { /* ignore */ }
}
setInterval(pollAlerts, 30000);
pollAlerts();

function renderAlerts(alerts) {
  const list  = document.getElementById('alerts-list');
  const count = document.getElementById('alert-count');
  alertLayer.clearLayers();

  if (!alerts.length) {
    list.innerHTML = '<span class="muted">No active alerts</span>';
    count.classList.add('hidden');
    return;
  }

  count.textContent = alerts.length;
  count.classList.remove('hidden');
  list.innerHTML = '';

  alerts.forEach(a => {
    // Map marker
    if (a.lat && a.lon) {
      L.circleMarker([a.lat, a.lon], {
        radius: 10, color: ALERT_COLOR, fillColor: ALERT_COLOR,
        fillOpacity: 0.7, weight: 2
      }).bindPopup(`<b>STALL ALERT</b><br>${a.timestamp.slice(0, 16).replace('T', ' ')}`)
        .addTo(alertLayer);
    }

    // Sidebar item
    const div = document.createElement('div');
    div.className = 'alert-item';
    const loc = (a.lat && a.lon) ? `${a.lat.toFixed(5)}, ${a.lon.toFixed(5)}` : 'Location unknown';
    div.innerHTML = `
      <div class="alert-loc">⚠ IRRIGATOR STALLED</div>
      <div class="alert-time">${a.timestamp.slice(0, 16).replace('T', ' ')} UTC</div>
      <div style="font-size:11px;color:#aaa">${loc}</div>
      <button class="btn btn-gray btn-ack" data-id="${a.id}">Acknowledge</button>`;
    list.appendChild(div);
  });

  list.querySelectorAll('.btn-ack').forEach(btn => {
    btn.addEventListener('click', async () => {
      await fetch(`${API}/api/alerts/${btn.dataset.id}/ack`, { method: 'PUT' });
      pollAlerts();
    });
  });
}

// ── New session modal ─────────────────────────────────────────────────────────
document.getElementById('btn-new-session').addEventListener('click', () => {
  document.getElementById('modal-overlay').classList.remove('hidden');
});
document.getElementById('btn-cancel-modal').addEventListener('click', () => {
  document.getElementById('modal-overlay').classList.add('hidden');
});
document.getElementById('btn-create-session').addEventListener('click', async () => {
  const name  = document.getElementById('new-name').value  || `Run ${new Date().toLocaleDateString()}`;
  const width = parseFloat(document.getElementById('new-width').value) || 30;
  const flow  = parseFloat(document.getElementById('new-flow').value)  || 833;
  const res   = await fetch(`${API}/api/sessions`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name, spread_width_m: width, flow_rate_lpm: flow })
  });
  if (res.ok) {
    document.getElementById('modal-overlay').classList.add('hidden');
    await loadSessions();
  }
});

// ── End session ───────────────────────────────────────────────────────────────
document.getElementById('btn-end-session').addEventListener('click', async () => {
  if (!activeSessionId) return;
  const ts = new Date().toISOString();
  await fetch(`${API}/api/sessions/${activeSessionId}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ end_time: ts })
  });
  await loadSessions();
});

// ── Settings ──────────────────────────────────────────────────────────────────
(async () => {
  const res = await fetch(`${API}/api/settings`);
  if (!res.ok) return;
  const s = await res.json();
  document.getElementById('cfg-width').value    = s.spread_width_m || 30;
  document.getElementById('cfg-flow').value     = s.flow_rate_lpm  || 833;
  document.getElementById('cfg-po-token').value = s.pushover_token || '';
  document.getElementById('cfg-po-user').value  = s.pushover_user  || '';
})();

document.getElementById('btn-save-settings').addEventListener('click', async () => {
  await fetch(`${API}/api/settings`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      spread_width_m: document.getElementById('cfg-width').value,
      flow_rate_lpm:  document.getElementById('cfg-flow').value,
      pushover_token: document.getElementById('cfg-po-token').value,
      pushover_user:  document.getElementById('cfg-po-user').value,
    })
  });
  const btn = document.getElementById('btn-save-settings');
  btn.textContent = 'Saved ✓';
  setTimeout(() => btn.textContent = 'Save Settings', 2000);
});
