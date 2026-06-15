#!/usr/bin/env python3
"""
LoRa Bridge Serial Receiver
Reads JSON lines from the ESP32 lora_bridge on USB serial,
writes positions and alerts to SQLite, sends Pushover notifications.

Usage:
    python3 receiver.py [/dev/ttyUSB0]
"""

import sys
import json
import time
import datetime
import requests
import serial
from database import get_conn, init_db, get_setting, get_active_session_id, create_session

SERIAL_PORT  = '/dev/ttyUSB0'
BAUD_RATE    = 115200

MSG_PUMP_ON      = 0x10
MSG_PUMP_OFF     = 0x11
MSG_HEARTBEAT    = 0x20
MSG_GPS_POSITION = 0x30
MSG_ALERT_STALL  = 0x40


def now_iso():
    return datetime.datetime.utcnow().isoformat()


def send_pushover(title, message, priority=1):
    token = get_setting('pushover_token', '')
    user  = get_setting('pushover_user',  '')
    if not token or not user:
        return
    try:
        requests.post(
            'https://api.pushover.net/1/messages.json',
            data={'token': token, 'user': user,
                  'title': title, 'message': message, 'priority': priority},
            timeout=10
        )
        print(f"[PUSHOVER] Sent: {title}")
    except Exception as e:
        print(f"[PUSHOVER] Error: {e}")


def handle_gps(pkt):
    session_id = get_active_session_id()
    conn = get_conn()
    conn.execute(
        '''INSERT INTO positions
           (device_id, timestamp, lat, lon, speed_cms, battery_pct,
            pump_on, session_id, rssi, snr)
           VALUES (?,?,?,?,?,?,?,?,?,?)''',
        (pkt.get('src', 2), now_iso(),
         pkt['lat'], pkt['lon'],
         pkt.get('speed', 0),
         pkt.get('battery', 0),
         pkt.get('pump_on', 0),
         session_id,
         pkt.get('rssi', 0),
         pkt.get('snr', 0.0))
    )
    conn.commit()
    conn.close()
    print(f"[GPS] {pkt['lat']:.6f}, {pkt['lon']:.6f}  "
          f"spd={pkt.get('speed',0)/100:.1f}m/s  "
          f"batt={pkt.get('battery',0)}%  RSSI={pkt.get('rssi',0)}")


def handle_pump_on():
    session_id = get_active_session_id()
    if session_id is None:
        date_str  = datetime.date.today().isoformat()
        session_id = create_session(f"Run {date_str}")
        print(f"[SESSION] Auto-created session {session_id}")
    print(f"[PUMP] ON — session {session_id}")


def handle_pump_off():
    print("[PUMP] OFF")


def handle_alert(pkt):
    alert_type = 'STALL' if pkt.get('type') == MSG_ALERT_STALL else 'UNKNOWN'
    lat = pkt.get('lat')
    lon = pkt.get('lon')

    conn = get_conn()
    conn.execute(
        'INSERT INTO alerts (device_id, timestamp, alert_type, lat, lon) VALUES (?,?,?,?,?)',
        (pkt.get('src', 2), now_iso(), alert_type, lat, lon)
    )
    conn.commit()
    conn.close()

    loc = f"{lat:.5f}, {lon:.5f}" if lat is not None else "unknown location"
    print(f"[ALERT] {alert_type} at {loc}")
    send_pushover(
        "Irrigator Stall Alert",
        f"Irrigator has not moved for 10+ minutes while pump is running.\nLocation: {loc}",
        priority=1
    )


def dispatch(pkt):
    msg_type = pkt.get('type')
    if msg_type == MSG_GPS_POSITION:
        handle_gps(pkt)
    elif msg_type == MSG_PUMP_ON:
        handle_pump_on()
    elif msg_type == MSG_PUMP_OFF:
        handle_pump_off()
    elif msg_type == MSG_ALERT_STALL:
        handle_alert(pkt)
    elif msg_type == MSG_HEARTBEAT:
        print(f"[HB] src={pkt.get('src')} pump={pkt.get('payload')} RSSI={pkt.get('rssi')}")
    else:
        print(f"[RX] Unknown type 0x{msg_type:02X}: {pkt}")


def run(port):
    init_db()
    print(f"[START] Listening on {port} at {BAUD_RATE} baud")
    backoff = 2

    while True:
        try:
            with serial.Serial(port, BAUD_RATE, timeout=10) as ser:
                print(f"[SERIAL] Connected to {port}")
                backoff = 2
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode('utf-8', errors='ignore').strip()
                    if not line or line.startswith('#'):
                        continue
                    try:
                        pkt = json.loads(line)
                        dispatch(pkt)
                    except json.JSONDecodeError:
                        print(f"[RAW] {line}")

        except serial.SerialException as e:
            print(f"[SERIAL] {e} — retry in {backoff}s")
            time.sleep(backoff)
            backoff = min(backoff * 2, 60)
        except KeyboardInterrupt:
            print("\n[STOP] Receiver stopped")
            break


if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) > 1 else SERIAL_PORT
    run(port)
