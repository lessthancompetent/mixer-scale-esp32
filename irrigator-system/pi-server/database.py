import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'irrigator.db')


def get_conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    return conn


def init_db():
    conn = get_conn()
    conn.executescript('''
        CREATE TABLE IF NOT EXISTS sessions (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            name          TEXT,
            start_time    TEXT NOT NULL,
            end_time      TEXT,
            spread_width_m REAL NOT NULL DEFAULT 30.0,
            flow_rate_lpm  REAL NOT NULL DEFAULT 833.0,
            notes         TEXT
        );

        CREATE TABLE IF NOT EXISTS positions (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id  INTEGER NOT NULL,
            timestamp  TEXT    NOT NULL,
            lat        REAL    NOT NULL,
            lon        REAL    NOT NULL,
            speed_cms  INTEGER DEFAULT 0,
            battery_pct INTEGER DEFAULT 0,
            pump_on    INTEGER DEFAULT 0,
            session_id INTEGER REFERENCES sessions(id),
            rssi       INTEGER,
            snr        REAL,
            battery_soh INTEGER DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS alerts (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id    INTEGER NOT NULL,
            timestamp    TEXT    NOT NULL,
            alert_type   TEXT    NOT NULL,
            lat          REAL,
            lon          REAL,
            acknowledged INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS settings (
            key   TEXT PRIMARY KEY,
            value TEXT
        );

        INSERT OR IGNORE INTO settings VALUES ('spread_width_m',  '24.0');
        INSERT OR IGNORE INTO settings VALUES ('spread_inner_m',  '6.0');
        INSERT OR IGNORE INTO settings VALUES ('flow_rate_lpm',   '416.7');
        INSERT OR IGNORE INTO settings VALUES ('pushover_token', '');
        INSERT OR IGNORE INTO settings VALUES ('pushover_user',  '');
        INSERT OR IGNORE INTO settings VALUES ('stall_timeout_min', '10');
        INSERT OR IGNORE INTO settings VALUES ('fm_device_eui',  '');
        INSERT OR IGNORE INTO settings VALUES ('fm_cs_api_key',  '');

        CREATE TABLE IF NOT EXISTS fm_ingredients (
            id      INTEGER PRIMARY KEY AUTOINCREMENT,
            name    TEXT NOT NULL,
            dm_pct  REAL NOT NULL DEFAULT 0.0,
            active  INTEGER NOT NULL DEFAULT 1
        );

        CREATE TABLE IF NOT EXISTS fm_herds (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            name           TEXT NOT NULL,
            num_cows       INTEGER NOT NULL DEFAULT 0,
            meals_per_day  INTEGER NOT NULL DEFAULT 2
        );

        CREATE TABLE IF NOT EXISTS fm_ration_entries (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            herd_id        INTEGER NOT NULL REFERENCES fm_herds(id) ON DELETE CASCADE,
            ingredient_id  INTEGER NOT NULL REFERENCES fm_ingredients(id),
            kg_dm_per_cow  REAL NOT NULL DEFAULT 0.0
        );

        CREATE TABLE IF NOT EXISTS fm_feed_log (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            logged_at     TEXT NOT NULL DEFAULT (datetime('now')),
            herd_id       INTEGER,
            herd_name     TEXT,
            total_wet_kg  REAL,
            steps_json    TEXT
        );

        CREATE TABLE IF NOT EXISTS fm_feed_sessions (
            id               INTEGER PRIMARY KEY AUTOINCREMENT,
            logged_at        TEXT NOT NULL DEFAULT (datetime('now')),
            herd_idx         INTEGER,
            herd_name        TEXT,
            num_cows         INTEGER,
            total_loaded_kg  REAL,
            total_fed_out_kg REAL,
            residual_kg      REAL GENERATED ALWAYS AS (total_loaded_kg - total_fed_out_kg) VIRTUAL
        );

        CREATE TABLE IF NOT EXISTS fm_session_feeds (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id     INTEGER NOT NULL REFERENCES fm_feed_sessions(id) ON DELETE CASCADE,
            ingredient_idx INTEGER,
            ingredient_name TEXT,
            dm_pct         REAL
        );

        CREATE TABLE IF NOT EXISTS fm_inventory (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            received_at     TEXT NOT NULL DEFAULT (datetime('now')),
            ingredient_id   INTEGER REFERENCES fm_ingredients(id),
            ingredient_name TEXT,
            kg_delivered    REAL NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_pos_session   ON positions(session_id);
        CREATE INDEX IF NOT EXISTS idx_pos_timestamp ON positions(timestamp);
        CREATE INDEX IF NOT EXISTS idx_alerts_ack    ON alerts(acknowledged);
    ''')
    # Migration: add battery_soh to pre-existing positions tables
    cols = [r['name'] for r in conn.execute("PRAGMA table_info(positions)").fetchall()]
    if 'battery_soh' not in cols:
        conn.execute('ALTER TABLE positions ADD COLUMN battery_soh INTEGER DEFAULT 0')

    # Seed default feed ingredient library if table is empty
    if conn.execute('SELECT COUNT(*) FROM fm_ingredients').fetchone()[0] == 0:
        conn.executemany(
            'INSERT INTO fm_ingredients (name, dm_pct) VALUES (?,?)',
            [
                ('Maize Silage',      32.0),
                ('Grass Silage',      28.0),
                ('Pasture (fresh)',   18.0),
                ('Hay (grass)',       86.0),
                ('Lucerne Hay',       88.0),
                ('Palm Kernel (PKE)', 90.0),
                ('Barley Grain',      86.0),
                ('Maize Grain',       86.0),
                ('Wheat Grain',       88.0),
                ('Soybean Meal',      89.0),
                ('Canola Meal',       88.0),
                ('Brewers Grain',     22.0),
                ('Molasses',          75.0),
                ('Wheat Straw',       86.0),
            ]
        )

    conn.commit()
    conn.close()


def get_setting(key, default=None):
    conn = get_conn()
    row = conn.execute('SELECT value FROM settings WHERE key=?', (key,)).fetchone()
    conn.close()
    return row['value'] if row else default


def set_setting(key, value):
    conn = get_conn()
    conn.execute('INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)',
                 (key, str(value)))
    conn.commit()
    conn.close()


def get_active_session_id():
    conn = get_conn()
    row = conn.execute(
        'SELECT id FROM sessions WHERE end_time IS NULL ORDER BY start_time DESC LIMIT 1'
    ).fetchone()
    conn.close()
    return row['id'] if row else None


def get_or_create_today_session():
    """
    Returns the session ID for today, creating one if needed.
    Automatically closes any open sessions from previous days.
    Called on PUMP_ON events so each calendar day gets its own session.
    """
    import datetime
    today     = datetime.date.today().isoformat()
    now_iso   = datetime.datetime.utcnow().isoformat()
    conn      = get_conn()

    # Close stale sessions that started before today and were never closed
    conn.execute(
        "UPDATE sessions SET end_time=? WHERE end_time IS NULL AND date(start_time) < ?",
        (now_iso, today)
    )
    conn.commit()

    # Return today's session if one already exists
    row = conn.execute(
        "SELECT id FROM sessions WHERE date(start_time)=? ORDER BY start_time DESC LIMIT 1",
        (today,)
    ).fetchone()
    if row:
        conn.close()
        return row['id']

    # Create a new named session for today
    sw = float(get_setting('spread_width_m', 30.0))
    fr = float(get_setting('flow_rate_lpm',  833.0))
    cur = conn.execute(
        'INSERT INTO sessions (name, start_time, spread_width_m, flow_rate_lpm) VALUES (?,?,?,?)',
        (f"Run {today}", now_iso, sw, fr)
    )
    conn.commit()
    sid = cur.lastrowid
    conn.close()
    return sid


def create_session(name, spread_width_m=None, flow_rate_lpm=None):
    import datetime
    if spread_width_m is None:
        spread_width_m = float(get_setting('spread_width_m', 30.0))
    if flow_rate_lpm is None:
        flow_rate_lpm = float(get_setting('flow_rate_lpm', 833.0))
    now = datetime.datetime.utcnow().isoformat()
    conn = get_conn()
    cur = conn.execute(
        'INSERT INTO sessions (name, start_time, spread_width_m, flow_rate_lpm) VALUES (?,?,?,?)',
        (name, now, spread_width_m, flow_rate_lpm)
    )
    conn.commit()
    sid = cur.lastrowid
    conn.close()
    return sid
