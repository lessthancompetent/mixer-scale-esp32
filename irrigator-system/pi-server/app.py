#!/usr/bin/env python3
"""
Effluent Irrigator Map Server
Run with: python3 app.py
Access at: http://192.168.5.111:5000
"""

import json
import datetime
from flask import Flask, jsonify, request, render_template, abort
from database import (get_conn, init_db, get_setting, set_setting,
                       create_session, get_or_create_today_session)
from spread_calc import calculate_spread

app = Flask(__name__)
HOST = '0.0.0.0'
PORT = 5000

init_db()

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


@app.get('/api/sessions/all-spreads')
def all_spreads():
    """
    Returns spread polygon + stats for every session that has enough GPS data.
    Used by the map to display all historical sessions simultaneously.
    """
    conn     = get_conn()
    sessions = conn.execute('SELECT * FROM sessions ORDER BY start_time ASC').fetchall()
    results  = []

    for session in sessions:
        rows = conn.execute(
            'SELECT lat, lon, timestamp FROM positions '
            'WHERE session_id=? AND pump_on=1 ORDER BY timestamp',
            (session['id'],)
        ).fetchall()

        if len(rows) < 2:
            continue

        result = calculate_spread(
            [dict(r) for r in rows],
            spread_width_m = session['spread_width_m'],
            flow_rate_lpm  = session['flow_rate_lpm'],
            spread_inner_m = float(get_setting('spread_inner_m', 6.0))
        )
        if result:
            results.append({
                'session': dict(session),
                'geojson': result['geojson'],
                'stats':   result['stats'],
            })

    conn.close()
    return jsonify(results)


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

    MSG_GPS    = 0x30
    MSG_ON     = 0x10
    MSG_OFF    = 0x11
    MSG_STALL  = 0x40
    MSG_CUTOFF = 0x50

    now = datetime.datetime.utcnow().isoformat()

    if msg_type == MSG_GPS:
        session_id = get_or_create_today_session()
        conn = get_conn()
        conn.execute(
            '''INSERT INTO positions
               (device_id, timestamp, lat, lon, speed_cms,
                battery_pct, pump_on, session_id, battery_soh)
               VALUES (?,?,?,?,?,?,?,?,?)''',
            (src, now, data.get('lat'), data.get('lon'),
             data.get('speed', 0), data.get('battery', 0),
             data.get('pump_on', 0), session_id, data.get('batt_soh', 0))
        )
        conn.commit()
        conn.close()
        return jsonify({'ok': True, 'session_id': session_id})

    if msg_type == MSG_ON:
        session_id = get_or_create_today_session()
        return jsonify({'ok': True, 'session_id': session_id})

    if msg_type == MSG_OFF:
        return jsonify({'ok': True})

    if msg_type in (MSG_STALL, MSG_CUTOFF):
        alert_type = 'KICKOUT' if msg_type == MSG_CUTOFF else 'STALL'
        conn = get_conn()
        conn.execute(
            'INSERT INTO alerts (device_id, timestamp, alert_type, lat, lon) VALUES (?,?,?,?,?)',
            (src, now, alert_type, data.get('lat'), data.get('lon'))
        )
        conn.commit()
        conn.close()
        return jsonify({'ok': True, 'alert': alert_type})

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
        flow_rate_lpm  = session['flow_rate_lpm'],
        spread_inner_m = float(get_setting('spread_inner_m', 6.0))
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
               'pushover_user', 'stall_timeout_min', 'fm_device_eui', 'fm_cs_api_key'}
    for key, value in data.items():
        if key in allowed:
            set_setting(key, value)
    return jsonify({'updated': list(data.keys())})


# ── Feed Mixer ───────────────────────────────────────────────────────────────

@app.get('/feedmixer/ingredients')
def fm_list_ingredients():
    conn = get_conn()
    rows = conn.execute('SELECT * FROM fm_ingredients ORDER BY name').fetchall()
    conn.close()
    return jsonify(rows_to_list(rows))


@app.post('/feedmixer/ingredients')
def fm_create_ingredient():
    data = request.get_json(force=True, silent=True) or {}
    name = data.get('name', '').strip()
    if not name:
        abort(400)
    dm = float(data.get('dm_pct', 0))
    conn = get_conn()
    cur = conn.execute('INSERT INTO fm_ingredients (name, dm_pct) VALUES (?,?)', (name, dm))
    conn.commit()
    row = conn.execute('SELECT * FROM fm_ingredients WHERE id=?', (cur.lastrowid,)).fetchone()
    conn.close()
    return jsonify(row_to_dict(row)), 201


@app.put('/feedmixer/ingredients/<int:iid>')
def fm_update_ingredient(iid):
    data = request.get_json(force=True, silent=True) or {}
    fields, vals = [], []
    for col in ('name', 'dm_pct', 'active'):
        if col in data:
            fields.append(f'{col}=?')
            vals.append(data[col])
    if not fields:
        abort(400)
    vals.append(iid)
    conn = get_conn()
    conn.execute(f'UPDATE fm_ingredients SET {", ".join(fields)} WHERE id=?', vals)
    conn.commit()
    row = conn.execute('SELECT * FROM fm_ingredients WHERE id=?', (iid,)).fetchone()
    conn.close()
    return jsonify(row_to_dict(row))


@app.delete('/feedmixer/ingredients/<int:iid>')
def fm_delete_ingredient(iid):
    conn = get_conn()
    conn.execute('DELETE FROM fm_ration_entries WHERE ingredient_id=?', (iid,))
    conn.execute('DELETE FROM fm_ingredients WHERE id=?', (iid,))
    conn.commit()
    conn.close()
    return jsonify({'deleted': iid})


@app.get('/feedmixer/herds')
def fm_list_herds():
    conn = get_conn()
    herds = rows_to_list(conn.execute('SELECT * FROM fm_herds ORDER BY name').fetchall())
    for h in herds:
        h['entries'] = rows_to_list(conn.execute(
            '''SELECT e.*, i.name AS ingredient_name, i.dm_pct
               FROM fm_ration_entries e
               JOIN fm_ingredients i ON i.id = e.ingredient_id
               WHERE e.herd_id=? ORDER BY e.id''', (h['id'],)
        ).fetchall())
    conn.close()
    return jsonify(herds)


@app.post('/feedmixer/herds')
def fm_create_herd():
    data = request.get_json(force=True, silent=True) or {}
    name = data.get('name', '').strip()
    if not name:
        abort(400)
    conn = get_conn()
    cur = conn.execute(
        'INSERT INTO fm_herds (name, num_cows, meals_per_day) VALUES (?,?,?)',
        (name, int(data.get('num_cows', 0)), int(data.get('meals_per_day', 2)))
    )
    conn.commit()
    row = conn.execute('SELECT * FROM fm_herds WHERE id=?', (cur.lastrowid,)).fetchone()
    conn.close()
    return jsonify(row_to_dict(row)), 201


@app.put('/feedmixer/herds/<int:hid>')
def fm_update_herd(hid):
    data = request.get_json(force=True, silent=True) or {}
    fields, vals = [], []
    for col in ('name', 'num_cows', 'meals_per_day'):
        if col in data:
            fields.append(f'{col}=?')
            vals.append(data[col])
    if not fields:
        abort(400)
    vals.append(hid)
    conn = get_conn()
    conn.execute(f'UPDATE fm_herds SET {", ".join(fields)} WHERE id=?', vals)
    conn.commit()
    row = conn.execute('SELECT * FROM fm_herds WHERE id=?', (hid,)).fetchone()
    conn.close()
    return jsonify(row_to_dict(row))


@app.delete('/feedmixer/herds/<int:hid>')
def fm_delete_herd(hid):
    conn = get_conn()
    conn.execute('DELETE FROM fm_ration_entries WHERE herd_id=?', (hid,))
    conn.execute('DELETE FROM fm_herds WHERE id=?', (hid,))
    conn.commit()
    conn.close()
    return jsonify({'deleted': hid})


@app.put('/feedmixer/herds/<int:hid>/ration')
def fm_set_ration(hid):
    entries = request.get_json(force=True, silent=True) or []
    conn = get_conn()
    conn.execute('DELETE FROM fm_ration_entries WHERE herd_id=?', (hid,))
    for e in entries:
        conn.execute(
            'INSERT INTO fm_ration_entries (herd_id, ingredient_id, kg_dm_per_cow) VALUES (?,?,?)',
            (hid, int(e['ingredient_id']), float(e['kg_dm_per_cow']))
        )
    conn.commit()
    rows = rows_to_list(conn.execute(
        '''SELECT e.*, i.name AS ingredient_name, i.dm_pct
           FROM fm_ration_entries e
           JOIN fm_ingredients i ON i.id = e.ingredient_id
           WHERE e.herd_id=? ORDER BY e.id''', (hid,)
    ).fetchall())
    conn.close()
    return jsonify(rows)


@app.get('/feedmixer/log')
def fm_get_log():
    limit = min(int(request.args.get('limit', 100)), 500)
    conn = get_conn()
    rows = rows_to_list(conn.execute(
        'SELECT * FROM fm_feed_log ORDER BY logged_at DESC LIMIT ?', (limit,)
    ).fetchall())
    conn.close()
    for r in rows:
        if r.get('steps_json'):
            r['steps'] = json.loads(r['steps_json'])
    return jsonify(rows)


@app.post('/feedmixer/log')
def fm_add_log():
    data = request.get_json(force=True, silent=True) or {}
    conn = get_conn()
    conn.execute(
        'INSERT INTO fm_feed_log (herd_id, herd_name, total_wet_kg, steps_json) VALUES (?,?,?,?)',
        (data.get('herd_id'), data.get('herd_name', ''),
         data.get('total_wet_kg', 0), json.dumps(data.get('steps', [])))
    )
    conn.commit()
    conn.close()
    return jsonify({'ok': True}), 201


# ── Feed sessions (v2 log) ────────────────────────────────────────────────────

@app.get('/feedmixer/logv2')
def fm_get_sessions():
    limit = min(int(request.args.get('limit', 100)), 500)
    conn  = get_conn()
    sessions = rows_to_list(conn.execute(
        'SELECT * FROM fm_feed_sessions ORDER BY logged_at DESC LIMIT ?', (limit,)
    ).fetchall())
    for s in sessions:
        s['feeds'] = rows_to_list(conn.execute(
            'SELECT * FROM fm_session_feeds WHERE session_id=? ORDER BY id', (s['id'],)
        ).fetchall())
        loaded = s.get('total_loaded_kg') or 0
        fedout = s.get('total_fed_out_kg') or 0
        s['residual_kg'] = round(loaded - fedout, 1)
    conn.close()
    return jsonify(sessions)


@app.post('/feedmixer/logv2')
def fm_add_session():
    data  = request.get_json(force=True, silent=True) or {}
    conn  = get_conn()
    # Resolve herd name from herds table if not supplied
    herd_name = data.get('herd_name', '')
    if not herd_name:
        row = conn.execute('SELECT name FROM fm_herds WHERE id=?', (data.get('herd_idx'),)).fetchone()
        if row: herd_name = row['name']
    cur = conn.execute(
        '''INSERT INTO fm_feed_sessions (herd_idx, herd_name, num_cows, total_loaded_kg, total_fed_out_kg)
           VALUES (?,?,?,?,?)''',
        (data.get('herd_idx'), herd_name, data.get('num_cows', 0),
         data.get('total_loaded_kg', 0), data.get('total_fed_out_kg', 0))
    )
    sid = cur.lastrowid
    for ing in (data.get('ingredients') or []):
        irow = conn.execute('SELECT name FROM fm_ingredients WHERE id=?',
                            (ing.get('ingredient_idx'),)).fetchone()
        iname = irow['name'] if irow else f"Ing {ing.get('ingredient_idx','?')}"
        conn.execute(
            'INSERT INTO fm_session_feeds (session_id, ingredient_idx, ingredient_name, dm_pct) VALUES (?,?,?,?)',
            (sid, ing.get('ingredient_idx'), iname, ing.get('dm_pct', 0))
        )
    conn.commit()
    conn.close()
    return jsonify({'ok': True, 'id': sid}), 201


# ── Feed inventory ─────────────────────────────────────────────────────────────

@app.get('/feedmixer/inventory')
def fm_get_inventory():
    conn  = get_conn()
    # Deliveries
    deliveries = rows_to_list(conn.execute(
        'SELECT * FROM fm_inventory ORDER BY received_at DESC LIMIT 500'
    ).fetchall())
    # Per-ingredient stock: deliveries minus estimated consumption
    # Consumption estimated from sessions: fed_out × (ingredient wet proportion)
    stock_rows = rows_to_list(conn.execute('''
        SELECT i.id, i.name, i.dm_pct,
               COALESCE(SUM(inv.kg_delivered),0) AS total_delivered
        FROM fm_ingredients i
        LEFT JOIN fm_inventory inv ON inv.ingredient_id = i.id
        GROUP BY i.id
    ''').fetchall())
    conn.close()
    return jsonify({'deliveries': deliveries, 'stock': stock_rows})


@app.post('/feedmixer/inventory')
def fm_add_delivery():
    data = request.get_json(force=True, silent=True) or {}
    ing_id = data.get('ingredient_id')
    kg     = float(data.get('kg_delivered', 0))
    if not ing_id or kg <= 0:
        abort(400)
    conn  = get_conn()
    row   = conn.execute('SELECT name FROM fm_ingredients WHERE id=?', (ing_id,)).fetchone()
    iname = row['name'] if row else ''
    conn.execute(
        'INSERT INTO fm_inventory (ingredient_id, ingredient_name, kg_delivered) VALUES (?,?,?)',
        (ing_id, iname, kg)
    )
    conn.commit()
    conn.close()
    return jsonify({'ok': True}), 201


@app.delete('/feedmixer/inventory/<int:did>')
def fm_delete_delivery(did):
    conn = get_conn()
    conn.execute('DELETE FROM fm_inventory WHERE id=?', (did,))
    conn.commit()
    conn.close()
    return jsonify({'deleted': did})


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
