#!/usr/bin/env python3
"""
Effluent Irrigator Map Server
Run with: python3 app.py
Access at: http://192.168.5.111:5000
"""

import json
import datetime
from flask import Flask, jsonify, request, render_template, abort
from database import get_conn, init_db, get_setting, set_setting, create_session
from spread_calc import calculate_spread

app = Flask(__name__)
HOST = '0.0.0.0'
PORT = 5000


# ── Helpers ──────────────────────────────────────────────────────────────────

def row_to_dict(row):
    return dict(row) if row else None


def rows_to_list(rows):
    return [dict(r) for r in rows]


# ── Sessions ─────────────────────────────────────────────────────────────────

@app.get('/api/sessions')
def list_sessions():
    conn = get_conn()
    rows = conn.execute(
        '''SELECT s.*,
                  (SELECT COUNT(*) FROM positions p WHERE p.session_id = s.id) AS point_count
           FROM sessions s ORDER BY s.start_time DESC'''
    ).fetchall()
    conn.close()
    return jsonify(rows_to_list(rows))


@app.post('/api/sessions')
def new_session():
    data   = request.get_json(force=True, silent=True) or {}
    name   = data.get('name', f"Run {datetime.date.today().isoformat()}")
    sw     = float(data.get('spread_width_m',  get_setting('spread_width_m', 30.0)))
    fr     = float(data.get('flow_rate_lpm',   get_setting('flow_rate_lpm',  833.0)))
    sid    = create_session(name, sw, fr)
    conn   = get_conn()
    row    = conn.execute('SELECT * FROM sessions WHERE id=?', (sid,)).fetchone()
    conn.close()
    return jsonify(row_to_dict(row)), 201


@app.put('/api/sessions/<int:sid>')
def update_session(sid):
    data = request.get_json(force=True, silent=True) or {}
    fields, vals = [], []
    for col in ('name', 'end_time', 'spread_width_m', 'flow_rate_lpm', 'notes'):
        if col in data:
            fields.append(f'{col}=?')
            vals.append(data[col])
    if not fields:
        abort(400)
    vals.append(sid)
    conn = get_conn()
    conn.execute(f'UPDATE sessions SET {", ".join(fields)} WHERE id=?', vals)
    conn.commit()
    row = conn.execute('SELECT * FROM sessions WHERE id=?', (sid,)).fetchone()
    conn.close()
    return jsonify(row_to_dict(row))


@app.delete('/api/sessions/<int:sid>')
def delete_session(sid):
    conn = get_conn()
    conn.execute('DELETE FROM positions WHERE session_id=?', (sid,))
    conn.execute('DELETE FROM sessions WHERE id=?', (sid,))
    conn.commit()
    conn.close()
    return jsonify({'deleted': sid})


# ── Positions ─────────────────────────────────────────────────────────────────

@app.get('/api/sessions/<int:sid>/positions')
def session_positions(sid):
    conn = get_conn()
    rows = conn.execute(
        'SELECT * FROM positions WHERE session_id=? ORDER BY timestamp',
        (sid,)
    ).fetchall()
    conn.close()
    return jsonify(rows_to_list(rows))


@app.get('/api/live')
def live_position():
    """Latest GPS fix from the irrigator (device_id=2)."""
    conn = get_conn()
    row  = conn.execute(
        'SELECT * FROM positions WHERE device_id=2 ORDER BY timestamp DESC LIMIT 1'
    ).fetchone()
    conn.close()
    return jsonify(row_to_dict(row))


@app.post('/api/ingest')
def ingest():
    """
    Direct HTTP ingest endpoint for WiFi-connected bench-test devices (e.g. T3).
    Accepts JSON matching the LoRa packet fields so the same DB schema is used.
    {
      "device_id": 2,
      "type": 48,        // 0x30=GPS, 0x10=PUMP_ON, 0x11=PUMP_OFF, 0x40=STALL
      "lat": -45.123456,
      "lon": 168.123456,
      "speed": 50,       // cm/s
      "battery": 85,
      "pump_on": 1
    }
    """
    data     = request.get_json(force=True, silent=True) or {}
    msg_type = data.get('type', 0x30)
    src      = data.get('device_id', 2)

    MSG_GPS   = 0x30
    MSG_ON    = 0x10
    MSG_OFF   = 0x11
    MSG_STALL = 0x40

    now = datetime.datetime.utcnow().isoformat()

    if msg_type == MSG_GPS:
        session_id = get_active_session_id()
        conn = get_conn()
        conn.execute(
            '''INSERT INTO positions
               (device_id, timestamp, lat, lon, speed_cms,
                battery_pct, pump_on, session_id)
               VALUES (?,?,?,?,?,?,?,?)''',
            (src, now, data.get('lat'), data.get('lon'),
             data.get('speed', 0), data.get('battery', 0),
             data.get('pump_on', 0), session_id)
        )
        conn.commit()
        conn.close()
        return jsonify({'ok': True, 'session_id': session_id})

    if msg_type == MSG_ON:
        session_id = get_active_session_id()
        if session_id is None:
            import database as _db
            session_id = _db.create_session(f"Run {datetime.date.today().isoformat()}")
        return jsonify({'ok': True, 'session_id': session_id})

    if msg_type == MSG_OFF:
        return jsonify({'ok': True})

    if msg_type == MSG_STALL:
        conn = get_conn()
        conn.execute(
            'INSERT INTO alerts (device_id, timestamp, alert_type, lat, lon) VALUES (?,?,?,?,?)',
            (src, now, 'STALL', data.get('lat'), data.get('lon'))
        )
        conn.commit()
        conn.close()
        return jsonify({'ok': True})

    return jsonify({'error': 'unknown type'}), 400


# ── Spread polygon ────────────────────────────────────────────────────────────

@app.get('/api/sessions/<int:sid>/spread')
def session_spread(sid):
    conn = get_conn()
    session = conn.execute('SELECT * FROM sessions WHERE id=?', (sid,)).fetchone()
    if not session:
        conn.close()
        abort(404)

    rows = conn.execute(
        'SELECT lat, lon, timestamp FROM positions WHERE session_id=? AND pump_on=1 ORDER BY timestamp',
        (sid,)
    ).fetchall()
    conn.close()

    positions = [dict(r) for r in rows]
    if len(positions) < 2:
        return jsonify({'error': 'Not enough positions to calculate spread'}), 422

    result = calculate_spread(
        positions,
        spread_width_m = session['spread_width_m'],
        flow_rate_lpm  = session['flow_rate_lpm']
    )
    if result is None:
        return jsonify({'error': 'Calculation failed'}), 500

    return jsonify(result)


# ── Alerts ────────────────────────────────────────────────────────────────────

@app.get('/api/alerts')
def list_alerts():
    ack    = request.args.get('ack', 'false').lower() == 'true'
    conn   = get_conn()
    rows   = conn.execute(
        'SELECT * FROM alerts WHERE acknowledged=? ORDER BY timestamp DESC LIMIT 50',
        (1 if ack else 0,)
    ).fetchall()
    conn.close()
    return jsonify(rows_to_list(rows))


@app.put('/api/alerts/<int:aid>/ack')
def ack_alert(aid):
    conn = get_conn()
    conn.execute('UPDATE alerts SET acknowledged=1 WHERE id=?', (aid,))
    conn.commit()
    conn.close()
    return jsonify({'acknowledged': aid})


# ── Settings ──────────────────────────────────────────────────────────────────

@app.get('/api/settings')
def get_settings():
    conn  = get_conn()
    rows  = conn.execute('SELECT key, value FROM settings').fetchall()
    conn.close()
    return jsonify({r['key']: r['value'] for r in rows})


@app.put('/api/settings')
def update_settings():
    data = request.get_json(force=True, silent=True) or {}
    allowed = {'spread_width_m', 'flow_rate_lpm', 'pushover_token',
               'pushover_user', 'stall_timeout_min'}
    for key, value in data.items():
        if key in allowed:
            set_setting(key, value)
    return jsonify({'updated': list(data.keys())})


# ── Root ──────────────────────────────────────────────────────────────────────

@app.get('/')
def index():
    return render_template('index.html',
                           server_host='192.168.5.111',
                           server_port=PORT)


if __name__ == '__main__':
    init_db()
    print(f"Irrigator map server at http://192.168.5.111:{PORT}")
    app.run(host=HOST, port=PORT, debug=False)
