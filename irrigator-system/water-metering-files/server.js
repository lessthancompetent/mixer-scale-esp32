'use strict';

const express      = require('express');
const session      = require('express-session');
const cookieParser = require('cookie-parser');
const bcrypt       = require('bcryptjs');
const Database     = require('better-sqlite3');
const mqtt         = require('mqtt');
const path         = require('path');
const os           = require('os');
const fs           = require('fs');
const ExcelJS      = require('exceljs');
const nodemailer   = require('nodemailer');
const schedule     = require('node-schedule');
const { createProxyMiddleware } = require('http-proxy-middleware');

const PORT            = process.env.PORT            || 3000;
const SECRET          = process.env.SESSION_SECRET  || 'change-this-secret-in-production';
const MQTT_URL        = process.env.MQTT_URL        || 'mqtt://localhost:1883';
const CHIRPSTACK_APP  = process.env.CHIRPSTACK_APP  || '870555a3-ebf5-4e4b-b629-0fdc3d3d3128';
const CS_HOST         = process.env.CS_HOST         || '127.0.0.1';
const CS_PORT         = parseInt(process.env.CS_PORT || '8080');
const TG_TOKEN        = process.env.TG_TOKEN        || '';
const TG_CHAT_ID      = process.env.TG_CHAT_ID      || '';
const SMTP_USER       = process.env.SMTP_USER       || '';
const SMTP_PASS       = process.env.SMTP_PASS       || '';
const REPORT_TO       = process.env.REPORT_TO       || 'you@example.com';

// ─── Database ─────────────────────────────────────────────────────────────────
const db = new Database(path.join(__dirname, 'db', 'tap.db'));
db.pragma('journal_mode = WAL');

db.exec(`
  CREATE TABLE IF NOT EXISTS users (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT UNIQUE NOT NULL,
    password TEXT NOT NULL
  );
  CREATE TABLE IF NOT EXISTS readings (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at  TEXT NOT NULL DEFAULT (datetime('now')),
    device_id    TEXT,
    event_type   INTEGER,
    event_name   TEXT,
    flags        INTEGER,
    tap_open     INTEGER,
    solenoid_on  INTEGER,
    alert_active INTEGER,
    temp_c       REAL,
    mins_open    INTEGER,
    mins_log     INTEGER,
    raw_payload  TEXT
  );
  CREATE TABLE IF NOT EXISTS alerts (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    triggered_at TEXT NOT NULL DEFAULT (datetime('now')),
    device_id    TEXT,
    temp_c       REAL,
    mins_open    INTEGER,
    acknowledged INTEGER DEFAULT 0
  );
  CREATE TABLE IF NOT EXISTS water_readings (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at  TEXT NOT NULL DEFAULT (datetime('now')),
    device_id    TEXT,
    event_type   INTEGER,
    tank_pulses  INTEGER NOT NULL DEFAULT 0,
    farm_pulses  INTEGER NOT NULL DEFAULT 0,
    raw_payload  TEXT
  );

  CREATE TABLE IF NOT EXISTS devices (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL,
    dev_eui    TEXT NOT NULL UNIQUE,
    type       TEXT NOT NULL DEFAULT 'other',
    app_eui    TEXT,
    app_key    TEXT,
    notes      TEXT,
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
  );
`);

const userCount = db.prepare('SELECT COUNT(*) as c FROM users').get().c;
if (userCount === 0) {
  const hash = bcrypt.hashSync('admin', 10);
  db.prepare('INSERT INTO users (username, password) VALUES (?, ?)').run('admin', hash);
  console.log('Default user created: admin / admin  ← CHANGE THIS PASSWORD');
}

// ─── Telegram ─────────────────────────────────────────────────────────────────
const https = require('https');

function sendTelegram(text) {
  if (!TG_TOKEN || !TG_CHAT_ID) return;
  const body = JSON.stringify({ chat_id: TG_CHAT_ID, text });
  const req = https.request({
    hostname: 'api.telegram.org',
    path: `/bot${TG_TOKEN}/sendMessage`,
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) },
  }, res => res.resume());
  req.on('error', err => console.error('Telegram error:', err.message));
  req.write(body); req.end();
}

// ─── Monthly shed report ─────────────────────────────────────────────────────────
const LITRES_PER_PULSE = parseInt(process.env.LITRES_PER_PULSE || '10');

async function generateAndSendReport(year, month) {
  const mm       = String(month).padStart(2, '0');
  const dateLabel = `${year}-${mm}`;
  const monthName = new Date(year, month - 1, 1)
    .toLocaleString('en-AU', { month: 'long', year: 'numeric' });
  const fileName  = `Water Take REDACTED-REF ${dateLabel}.xlsx`;
  const filePath  = path.join(os.tmpdir(), fileName);

  // Daily shed totals for the requested month
  const rows = db.prepare(`
    SELECT
      date(received_at) AS day,
      (SUM(tank_pulses) - SUM(farm_pulses)) * ${LITRES_PER_PULSE} AS shed_litres
    FROM water_readings
    WHERE strftime('%Y', received_at) = ? AND strftime('%m', received_at) = ?
    GROUP BY date(received_at)
    ORDER BY day ASC
  `).all(String(year), mm);

  // Build workbook
  const wb = new ExcelJS.Workbook();
  wb.creator = 'MVMS Water Metering';
  wb.created = new Date();

  const ws = wb.addWorksheet('Shed Water Use');

  ws.columns = [
    { header: 'Date',          key: 'day',         width: 16 },
    { header: 'Shed Use (L)',  key: 'shed_litres',  width: 18 },
  ];

  // Header style
  ws.getRow(1).eachCell(cell => {
    cell.font = { bold: true, color: { argb: 'FFFFFFFF' } };
    cell.fill = { type: 'pattern', pattern: 'solid', fgColor: { argb: 'FF1E3A5F' } };
    cell.alignment = { horizontal: 'center' };
  });

  // Data rows
  rows.forEach((r, i) => {
    const row = ws.addRow({ day: r.day, shed_litres: r.shed_litres });
    if (i % 2 === 1) {
      row.eachCell(cell => {
        cell.fill = { type: 'pattern', pattern: 'solid', fgColor: { argb: 'FFF0F4F8' } };
      });
    }
    row.getCell('shed_litres').alignment = { horizontal: 'right' };
  });

  // Total row
  const total = rows.reduce((s, r) => s + (r.shed_litres || 0), 0);
  const totalRow = ws.addRow({ day: 'TOTAL', shed_litres: total });
  totalRow.eachCell(cell => {
    cell.font = { bold: true };
    cell.border = { top: { style: 'thin' } };
  });
  totalRow.getCell('shed_litres').alignment = { horizontal: 'right' };

  await wb.xlsx.writeFile(filePath);

  if (!SMTP_USER || !SMTP_PASS) {
    console.warn(`[report] SMTP not configured — report saved to ${filePath}`);
    return;
  }

  const transporter = nodemailer.createTransport({
    service: 'gmail',
    auth: { user: SMTP_USER, pass: SMTP_PASS },
  });

  await transporter.sendMail({
    from:    SMTP_USER,
    to:      REPORT_TO,
    subject: `Water Take REDACTED-REF — ${monthName}`,
    text:    `Please find attached the daily shed water usage report for ${monthName}.\n\nTotal: ${total.toLocaleString()} L`,
    attachments: [{ filename: fileName, path: filePath }],
  });

  fs.unlinkSync(filePath);
  console.log(`[report] Monthly shed report sent: ${fileName} → ${REPORT_TO}`);
}

// Run at 06:00 on the 1st of every month; report covers the previous month
schedule.scheduleJob('0 6 1 * *', () => {
  const now  = new Date();
  const year = now.getMonth() === 0 ? now.getFullYear() - 1 : now.getFullYear();
  const month = now.getMonth() === 0 ? 12 : now.getMonth();
  generateAndSendReport(year, month)
    .catch(err => console.error('[report] Failed:', err.message));
});

// ─── MQTT ─────────────────────────────────────────────────────────────────────
const EVENT_NAMES = ['periodic', 'tap_open', 'tap_close', 'alert', 'log_end'];
const sseClients  = new Set();

function broadcastSSE(data) {
  const msg = `data: ${JSON.stringify(data)}\n\n`;
  for (const res of sseClients) {
    try { res.write(msg); } catch (_) { sseClients.delete(res); }
  }
}

const mqttClient = mqtt.connect(MQTT_URL);

mqttClient.on('connect', () => {
  const topic = `application/${CHIRPSTACK_APP}/device/+/event/up`;
  mqttClient.subscribe(topic, err => {
    if (err) console.error('MQTT subscribe error:', err);
    else console.log(`Subscribed to ${topic}`);
  });
});

// Forward decoded irrigator data to Flask ingest endpoint
function postToFlask(payload) {
  const body = JSON.stringify(payload);
  const req = require('http').request({
    hostname: '127.0.0.1', port: 5000, path: '/api/ingest',
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) },
  });
  req.on('error', err => console.error('[IRRIGATOR→FLASK]', err.message));
  req.write(body);
  req.end();
}

// Irrigator message types (0x10+ range, distinct from VAT tap 0x00-0x09)
const IRR_TYPES = new Set([0x10, 0x11, 0x20, 0x30, 0x40]);

mqttClient.on('message', (_topic, message) => {
  try {
    const body     = JSON.parse(message.toString());
    const deviceId = body?.deviceInfo?.devEui || 'unknown';
    const rawB64   = body?.data;
    const fPort    = body?.fPort || 1;
    if (!rawB64) return;
    const buf = Buffer.from(rawB64, 'base64');

    // Irrigator / T3 LoRaWAN packet (type byte ≥ 0x10)
    if (buf.length >= 2 && IRR_TYPES.has(buf[1])) {
      const msgType  = buf[1];
      const srcId    = buf[0];
      if (msgType === 0x30 && buf.length >= 14) {
        const lat    = buf.readInt32BE(2)  / 1e6;
        const lon    = buf.readInt32BE(6)  / 1e6;
        const speed  = buf.readInt16BE(10);
        const batt   = buf[12];
        const pumpOn = buf[13];
        postToFlask({ device_id: srcId, type: 0x30, lat, lon, speed, battery: batt, pump_on: pumpOn });
        console.log(`[IRR ${deviceId}] GPS ${lat.toFixed(5)},${lon.toFixed(5)} spd=${speed}cm/s pump=${pumpOn}`);
      } else if (msgType === 0x10 || msgType === 0x11) {
        postToFlask({ device_id: srcId, type: msgType });
        console.log(`[IRR ${deviceId}] PUMP ${msgType === 0x10 ? 'ON' : 'OFF'}`);
      } else if (msgType === 0x40 && buf.length >= 10) {
        const lat = buf.readInt32BE(2) / 1e6;
        const lon = buf.readInt32BE(6) / 1e6;
        postToFlask({ device_id: srcId, type: 0x40, lat, lon });
        console.log(`[IRR ${deviceId}] STALL at ${lat.toFixed(5)},${lon.toFixed(5)}`);
      }
      return;
    }

    if (fPort === 2) {
      if (buf.length < 9) return;
      const eventType  = buf[0];
      const tankPulses = buf.readUInt32BE(1);
      const farmPulses = buf.readUInt32BE(5);
      db.prepare(`
        INSERT INTO water_readings (device_id, event_type, tank_pulses, farm_pulses, raw_payload)
        VALUES (?,?,?,?,?)
      `).run(deviceId, eventType, tankPulses, farmPulses, rawB64);
      console.log(`[WATER ${deviceId}] tank+${tankPulses} farm+${farmPulses}`);
      broadcastSSE({ type: 'water', deviceId, tankPulses, farmPulses });
      return;
    }

    // Feed mixer feed-log uplink (fPort=11, type byte 0x50)
    if (fPort === 11 && buf.length >= 5 && buf[0] === 0x50) {
      const herdId     = buf[1];
      const totalWetKg = buf.readUInt16BE(2) / 10.0;
      const numSteps   = buf[4];
      const steps      = [];
      for (let i = 0; i < numSteps && (5 + i * 5 + 4) < buf.length; i++) {
        const off = 5 + i * 5;
        steps.push({
          ingredient_idx: buf[off],
          target_kg:      buf.readUInt16BE(off + 1) / 10.0,
          loaded_kg:      buf.readUInt16BE(off + 3) / 10.0,
        });
      }
      const logBody = JSON.stringify({ herd_id: herdId, total_wet_kg: totalWetKg, steps });
      const logReq  = require('http').request({
        hostname: '127.0.0.1', port: 5000, path: '/feedmixer/log',
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(logBody) },
      }, res => res.resume());
      logReq.on('error', err => console.error('[FEEDMIX→FLASK]', err.message));
      logReq.write(logBody); logReq.end();
      console.log(`[FEEDMIX ${deviceId}] herd=${herdId} total=${totalWetKg}kg steps=${numSteps}`);
      broadcastSSE({ type: 'feedmix_log', deviceId, herdId, totalWetKg, steps });
      return;
    }

    if (buf.length < 8) return;
    const eventType   = buf[0];
    const flags       = buf[1];
    const tempRaw     = buf.readInt16BE(2);
    const minsOpen    = buf.readUInt16BE(4);
    const minsLog     = buf.readUInt16BE(6);
    const tempC       = tempRaw / 100.0;
    const tapOpen     = (flags & 0x01) ? 1 : 0;
    const solenoidOn  = (flags & 0x02) ? 1 : 0;
    const alertActive = (flags & 0x04) ? 1 : 0;
    const eventName   = EVENT_NAMES[eventType] || 'unknown';

    db.prepare(`
      INSERT INTO readings
        (device_id, event_type, event_name, flags, tap_open, solenoid_on,
         alert_active, temp_c, mins_open, mins_log, raw_payload)
      VALUES (?,?,?,?,?,?,?,?,?,?,?)
    `).run(deviceId, eventType, eventName, flags, tapOpen, solenoidOn,
           alertActive, tempC, minsOpen, minsLog, rawB64);

    if (eventType === 3) {
      db.prepare('INSERT INTO alerts (device_id, temp_c, mins_open) VALUES (?,?,?)')
        .run(deviceId, tempC, minsOpen);
      console.log(`ALERT from ${deviceId}: ${tempC}°C at ${minsOpen} min`);
      sendTelegram(`🚨 VAT Tap Alert!\nTemp: ${tempC}°C\nOpen for: ${minsOpen} min\nDevice: ${deviceId}`);
    }

    console.log(`[${deviceId}] ${eventName} | ${tempC}°C | tap=${tapOpen}`);
    broadcastSSE({ type: 'vat', deviceId, eventName, tempC, tapOpen, solenoidOn, alertActive, minsOpen });
  } catch (err) {
    console.error('MQTT message error:', err);
  }
});

mqttClient.on('error', err => console.error('MQTT error:', err));

// ─── Express ────────────────────────────────────────────────────────────────────
const app = express();
app.use(cookieParser());
app.use(session({
  secret: SECRET, resave: false, saveUninitialized: false,
  cookie: { secure: false, maxAge: 8 * 60 * 60 * 1000 },
}));
app.use(express.static(path.join(__dirname, 'public')));

// Both proxies must be registered BEFORE express.json() so the request
// body stream is forwarded intact to Flask (body parsers consume the stream).
app.use('/irrigator-api', requireAuth, createProxyMiddleware({
  target: 'http://127.0.0.1:5000',
  changeOrigin: true,
  pathRewrite: { '^/': '/api/' },
  on: {
    error: (_err, _req, res) =>
      res.status(502).json({ error: 'Irrigator server unavailable — is Flask running on port 5000?' }),
  },
}));

app.use('/feedmixer-api', requireAuth, createProxyMiddleware({
  target: 'http://127.0.0.1:5000',
  changeOrigin: true,
  pathRewrite: { '^/': '/feedmixer/' },
  on: {
    error: (_err, _req, res) =>
      res.status(502).json({ error: 'Feed mixer server unavailable — is Flask running on port 5000?' }),
  },
}));

app.use(express.json());
app.use(express.urlencoded({ extended: false }));

function requireAuth(req, res, next) {
  if (req.session && req.session.userId) return next();
  res.status(401).json({ error: 'Unauthorized' });
}

// ─── Auth ─────────────────────────────────────────────────────────────────────
app.post('/api/login', (req, res) => {
  const { username, password } = req.body;
  const user = db.prepare('SELECT * FROM users WHERE username = ?').get(username);
  if (!user || !bcrypt.compareSync(password, user.password))
    return res.status(401).json({ error: 'Invalid credentials' });
  req.session.userId   = user.id;
  req.session.username = user.username;
  res.json({ ok: true, username: user.username });
});
app.post('/api/logout', (req, res) => { req.session.destroy(() => res.json({ ok: true })); });
app.get('/api/me', requireAuth, (req, res) => { res.json({ username: req.session.username }); });
app.post('/api/change-password', requireAuth, (req, res) => {
  const { currentPassword, newPassword } = req.body;
  if (!newPassword || newPassword.length < 6)
    return res.status(400).json({ error: 'Password must be at least 6 characters' });
  const user = db.prepare('SELECT * FROM users WHERE id = ?').get(req.session.userId);
  if (!bcrypt.compareSync(currentPassword, user.password))
    return res.status(401).json({ error: 'Current password incorrect' });
  db.prepare('UPDATE users SET password = ? WHERE id = ?')
    .run(bcrypt.hashSync(newPassword, 10), req.session.userId);
  res.json({ ok: true });
});

// ─── SSE ──────────────────────────────────────────────────────────────────────
app.get('/api/events', requireAuth, (req, res) => {
  res.setHeader('Content-Type', 'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection', 'keep-alive');
  res.flushHeaders();
  sseClients.add(res);
  req.on('close', () => sseClients.delete(res));
});

// ─── VAT tap API ───────────────────────────────────────────────────────────────
app.get('/api/readings', requireAuth, (req, res) => {
  const limit  = Math.min(parseInt(req.query.limit  || 200), 1000);
  const offset = parseInt(req.query.offset || 0);
  res.json(db.prepare('SELECT * FROM readings ORDER BY id DESC LIMIT ? OFFSET ?').all(limit, offset));
});
app.get('/api/readings/latest', requireAuth, (req, res) => {
  res.json(db.prepare('SELECT * FROM readings ORDER BY id DESC LIMIT 1').get() || null);
});
app.get('/api/readings/chart', requireAuth, (req, res) => {
  const hours = parseInt(req.query.hours || 2);
  const from  = req.query.from || null;
  const to    = req.query.to   || null;
  let rows;
  if (from && to) {
    rows = db.prepare(`
      SELECT received_at, temp_c, tap_open, solenoid_on, alert_active, event_name
      FROM readings WHERE received_at >= ? AND received_at <= ? ORDER BY id ASC
    `).all(from, to);
  } else {
    rows = db.prepare(`
      SELECT received_at, temp_c, tap_open, solenoid_on, alert_active, event_name
      FROM readings WHERE received_at >= datetime('now','-'||?||' hours') ORDER BY id ASC
    `).all(hours);
  }
  res.json(rows);
});
app.get('/api/alerts', requireAuth, (req, res) => {
  res.json(db.prepare('SELECT * FROM alerts ORDER BY id DESC LIMIT 50').all());
});
app.post('/api/alerts/:id/acknowledge', requireAuth, (req, res) => {
  db.prepare('UPDATE alerts SET acknowledged = 1 WHERE id = ?').run(req.params.id);
  res.json({ ok: true });
});

// ─── Water meter API ─────────────────────────────────────────────────────────
app.get('/api/water/readings', requireAuth, (req, res) => {
  const limit  = Math.min(parseInt(req.query.limit || 200), 1000);
  const offset = parseInt(req.query.offset || 0);
  res.json(db.prepare('SELECT * FROM water_readings ORDER BY id DESC LIMIT ? OFFSET ?').all(limit, offset));
});
app.get('/api/water/daily', requireAuth, (req, res) => {
  const days = parseInt(req.query.days || 30);
  const rows = db.prepare(`
    SELECT date(received_at) AS day,
      SUM(tank_pulses) AS tank_pulses, SUM(farm_pulses) AS farm_pulses,
      SUM(tank_pulses)-SUM(farm_pulses) AS shed_pulses
    FROM water_readings
    WHERE received_at >= date('now','-'||?||' days')
    GROUP BY date(received_at) ORDER BY day ASC
  `).all(days);
  res.json(rows.map(r => ({
    day:         r.day,
    tank_litres: r.tank_pulses * LITRES_PER_PULSE,
    farm_litres: r.farm_pulses * LITRES_PER_PULSE,
    shed_litres: r.shed_pulses * LITRES_PER_PULSE,
  })));
});
app.get('/api/water/totals', requireAuth, (req, res) => {
  const row = db.prepare('SELECT SUM(tank_pulses) AS tp, SUM(farm_pulses) AS fp FROM water_readings').get();
  res.json({
    tank_litres: (row.tp || 0) * LITRES_PER_PULSE,
    farm_litres: (row.fp || 0) * LITRES_PER_PULSE,
    shed_litres: ((row.tp || 0) - (row.fp || 0)) * LITRES_PER_PULSE,
  });
});

const BUCKET_SECS = { '5min':300,'10min':600,'30min':1800,'1hr':3600,'4hr':14400,'1day':86400 };
const WINDOW_HRS  = { '1hr':1,'6hr':6,'24hr':24,'3day':72,'7day':168,'30day':720 };

app.get('/api/water/chart', requireAuth, (req, res) => {
  const bKey  = BUCKET_SECS[req.query.bucket] ? req.query.bucket : '1hr';
  const wKey  = WINDOW_HRS[req.query.window]  ? req.query.window  : '24hr';
  const avg14 = req.query.avg14 === 'true';
  const bs    = BUCKET_SECS[bKey], wh = WINDOW_HRS[wKey];
  const bExpr = `datetime((strftime('%s',received_at)/${bs})*${bs},'unixepoch')`;

  const data = db.prepare(`
    SELECT ${bExpr} AS bucket, SUM(tank_pulses) AS tp, SUM(farm_pulses) AS fp
    FROM water_readings WHERE received_at >= datetime('now','-${wh} hours')
    GROUP BY ${bExpr} ORDER BY bucket ASC
  `).all().map(r => ({
    bucket:      r.bucket,
    tank_litres: r.tp * LITRES_PER_PULSE,
    farm_litres: r.fp * LITRES_PER_PULSE,
    shed_litres: (r.tp - r.fp) * LITRES_PER_PULSE,
    tank_lph:    Math.round(r.tp * LITRES_PER_PULSE / bs * 3600),
    farm_lph:    Math.round(r.fp * LITRES_PER_PULSE / bs * 3600),
  }));

  let avg14Data = null;
  if (avg14) {
    if (bs === 86400) {
      const row = db.prepare(`
        SELECT AVG(dt) AS avg_tank_litres, AVG(df) AS avg_farm_litres FROM (
          SELECT date(received_at) AS d,
            SUM(tank_pulses)*${LITRES_PER_PULSE} AS dt, SUM(farm_pulses)*${LITRES_PER_PULSE} AS df
          FROM water_readings WHERE received_at >= date('now','-14 days')
          GROUP BY date(received_at))
      `).get();
      avg14Data = (row && row.avg_tank_litres !== null)
        ? [{ slot:-1, avg_tank_litres:Math.round(row.avg_tank_litres),
             avg_farm_litres:Math.round(row.avg_farm_litres), avg_tank_lph:0, avg_farm_lph:0 }]
        : [];
    } else {
      avg14Data = db.prepare(`
        SELECT (strftime('%s',received_at)%86400)/${bs} AS slot,
          AVG(tank_pulses)*${LITRES_PER_PULSE} AS avg_tank_litres,
          AVG(farm_pulses)*${LITRES_PER_PULSE} AS avg_farm_litres
        FROM water_readings WHERE received_at >= datetime('now','-14 days')
        GROUP BY (strftime('%s',received_at)%86400)/${bs} ORDER BY slot ASC
      `).all().map(r => ({
        slot: r.slot,
        avg_tank_litres: Math.round(r.avg_tank_litres),
        avg_farm_litres: Math.round(r.avg_farm_litres),
        avg_tank_lph: Math.round(r.avg_tank_litres / bs * 3600),
        avg_farm_lph: Math.round(r.avg_farm_litres / bs * 3600),
      }));
    }
  }
  res.json({ data, avg14: avg14Data, bucketSecs: bs });
});

// Manual report trigger (for testing) — POST /api/water/report?year=2026&month=5
app.post('/api/water/report', requireAuth, async (req, res) => {
  const now   = new Date();
  const year  = parseInt(req.query.year  || (now.getMonth() === 0 ? now.getFullYear()-1 : now.getFullYear()));
  const month = parseInt(req.query.month || (now.getMonth() === 0 ? 12 : now.getMonth()));
  try {
    await generateAndSendReport(year, month);
    res.json({ ok: true, message: `Report for ${year}-${String(month).padStart(2,'0')} sent to ${REPORT_TO}` });
  } catch (err) {
    console.error('[report]', err);
    res.status(500).json({ error: err.message });
  }
});

// ─── Device registry ─────────────────────────────────────────────────────────

app.post('/api/verify-password', requireAuth, (req, res) => {
  const user = db.prepare('SELECT * FROM users WHERE id = ?').get(req.session.userId);
  if (!user || !bcrypt.compareSync(req.body.password || '', user.password))
    return res.status(401).json({ error: 'Incorrect password' });
  res.json({ ok: true });
});

app.get('/api/devices', requireAuth, (req, res) => {
  res.json(db.prepare('SELECT * FROM devices ORDER BY type, name').all());
});

app.post('/api/devices', requireAuth, (req, res) => {
  const { name, dev_eui, type, app_eui, app_key, notes } = req.body;
  if (!name || !dev_eui) return res.status(400).json({ error: 'name and dev_eui required' });
  try {
    const r = db.prepare(
      'INSERT INTO devices (name, dev_eui, type, app_eui, app_key, notes) VALUES (?,?,?,?,?,?)'
    ).run(name, dev_eui.toLowerCase(), type || 'other', app_eui || '', app_key || '', notes || '');
    res.json(db.prepare('SELECT * FROM devices WHERE id=?').get(r.lastInsertRowid));
  } catch (e) {
    res.status(409).json({ error: 'DevEUI already registered' });
  }
});

app.put('/api/devices/:id', requireAuth, (req, res) => {
  const { name, dev_eui, type, app_eui, app_key, notes } = req.body;
  const fields = [], vals = [];
  if (name    !== undefined) { fields.push('name=?');    vals.push(name); }
  if (dev_eui !== undefined) { fields.push('dev_eui=?'); vals.push(dev_eui.toLowerCase()); }
  if (type    !== undefined) { fields.push('type=?');    vals.push(type); }
  if (app_eui !== undefined) { fields.push('app_eui=?'); vals.push(app_eui); }
  if (app_key !== undefined) { fields.push('app_key=?'); vals.push(app_key); }
  if (notes   !== undefined) { fields.push('notes=?');   vals.push(notes); }
  if (!fields.length) return res.status(400).json({ error: 'nothing to update' });
  vals.push(req.params.id);
  db.prepare(`UPDATE devices SET ${fields.join(',')} WHERE id=?`).run(...vals);
  res.json(db.prepare('SELECT * FROM devices WHERE id=?').get(req.params.id));
});

app.delete('/api/devices/:id', requireAuth, (req, res) => {
  db.prepare('DELETE FROM devices WHERE id=?').run(req.params.id);
  res.json({ deleted: Number(req.params.id) });
});

// ─── Feed Mixer ───────────────────────────────────────────────────────────────

function postToFlaskFM(path, payload) {
  const body = JSON.stringify(payload);
  const req  = require('http').request({
    hostname: '127.0.0.1', port: 5000, path,
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) },
  }, res => res.resume());
  req.on('error', err => console.error('[FM→FLASK]', err.message));
  req.write(body); req.end();
}

function getFlaskFM(path) {
  return new Promise((resolve, reject) => {
    const req = require('http').request(
      { hostname: '127.0.0.1', port: 5000, path, method: 'GET' },
      res => {
        let data = '';
        res.on('data', chunk => { data += chunk; });
        res.on('end', () => {
          try { resolve(JSON.parse(data)); } catch (e) { reject(e); }
        });
      }
    );
    req.on('error', reject);
    req.end();
  });
}

function sendChirpStackDownlink(devEui, _apiKey, fPort, payloadBuf) {
  const topic = `application/${CHIRPSTACK_APP}/device/${devEui}/command/down`;
  const msg   = JSON.stringify({ devEui, confirmed: false, fPort, data: payloadBuf.toString('base64') });
  return new Promise((resolve, reject) => {
    mqttClient.publish(topic, msg, { qos: 0 }, err => {
      if (err) reject(err);
      else resolve({ status: 200, body: 'published via MQTT' });
    });
  });
}

function encodeIngredientPacket(idx, name, dmPct) {
  const buf  = Buffer.alloc(14);
  buf[0]     = 0x02;
  buf[1]     = idx & 0xff;
  const nameBytes = Buffer.from(name.slice(0, 10).padEnd(10, '\0'), 'utf8');
  nameBytes.copy(buf, 2, 0, 10);
  buf.writeUInt16BE(Math.round(dmPct * 10) & 0xffff, 12);
  return buf;
}

function encodeHerdPacket(idx, name, numCows, mealsPerDay, entries) {
  const maxEntries = Math.min(entries.length, 8);
  const buf  = Buffer.alloc(16 + maxEntries * 3);
  buf[0]     = 0x01;
  buf[1]     = idx & 0xff;
  const nameBytes = Buffer.from(name.slice(0, 10).padEnd(10, '\0'), 'utf8');
  nameBytes.copy(buf, 2, 0, 10);
  buf.writeUInt16BE(numCows & 0xffff, 12);
  buf[14]    = mealsPerDay & 0xff;
  buf[15]    = maxEntries & 0xff;
  for (let i = 0; i < maxEntries; i++) {
    const e   = entries[i];
    const off = 16 + i * 3;
    buf[off]  = (e.dl_idx !== undefined ? e.dl_idx : e.ingredient_id - 1) & 0xff;
    buf.writeUInt16BE(Math.round(e.kg_dm_per_cow * 100) & 0xffff, off + 1);
  }
  return buf;
}

// POST /api/feedmixer/push-device  { herd_ids: [1,2,...] }
app.post('/api/feedmixer/push-device', requireAuth, async (req, res) => {
  try {
    const settings = await getFlaskFM('/api/settings');
    const devEui   = (settings.fm_device_eui || '').trim();
    const apiKey   = (settings.fm_cs_api_key  || '').trim();
    if (!devEui) return res.status(400).json({ error: 'Mixer wagon DevEUI not configured in Feed Mixer settings.' });
    if (!apiKey) return res.status(400).json({ error: 'ChirpStack API key not configured in Feed Mixer settings.' });

    const herds       = await getFlaskFM('/feedmixer/herds');
    const ingredients = await getFlaskFM('/feedmixer/ingredients');

    const herdIds = Array.isArray(req.body.herd_ids)
      ? req.body.herd_ids.map(Number)
      : herds.map(h => h.id);

    const queued = [];
    const activeIngs = ingredients.filter(i => i.active);

    // Map ingredient DB id → downlink index
    const ingDlIdx = {};
    activeIngs.forEach((ing, i) => { ingDlIdx[ing.id] = i; });

    // Send each active ingredient
    for (let i = 0; i < activeIngs.length; i++) {
      const ing = activeIngs[i];
      const pkt = encodeIngredientPacket(i, ing.name, ing.dm_pct);
      const r = await sendChirpStackDownlink(devEui, apiKey, 10, pkt);
      console.log(`[FEEDMIX CS] ingredient[${i}]=${ing.name} status=${r.status} body=${r.body}`);
      if (r.status >= 200 && r.status < 300) queued.push(`ingredient[${i}]=${ing.name}`);
    }

    // Send each selected herd
    let herdIdx = 0;
    for (const herd of herds) {
      if (!herdIds.includes(herd.id)) { herdIdx++; continue; }
      const mappedEntries = (herd.entries || []).map(e => ({
        ...e, dl_idx: ingDlIdx[e.ingredient_id] ?? 0,
      }));
      const pkt = encodeHerdPacket(herdIdx, herd.name, herd.num_cows, herd.meals_per_day, mappedEntries);
      const r = await sendChirpStackDownlink(devEui, apiKey, 10, pkt);
      console.log(`[FEEDMIX CS] herd[${herdIdx}]=${herd.name} status=${r.status} body=${r.body}`);
      if (r.status >= 200 && r.status < 300) queued.push(`herd[${herdIdx}]=${herd.name}`);
      herdIdx++;
    }

    console.log(`[FEEDMIX] Queued ${queued.length} downlinks to ${devEui}`);
    res.json({ ok: true, queued });
  } catch (err) {
    console.error('[FEEDMIX push]', err);
    res.status(500).json({ error: err.message });
  }
});

// ─── Start ────────────────────────────────────────────────────────────────────
app.listen(PORT, () => {
  console.log(`MVMS VAT Tap Monitor listening on http://0.0.0.0:${PORT}`);
});
