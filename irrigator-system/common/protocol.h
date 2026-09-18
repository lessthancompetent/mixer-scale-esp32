// LoRa protocol shared by every module in irrigator-system.
// Plain C++ (no Arduino dependencies) so it can be unit-tested on the PC.
//
// Every packet is:  [src id] [msg type] [payload...]
#pragma once
#include <stdint.h>

// ── Radio settings (all modules must match) ─────────────────────────────────
#define PROTO_LORA_FREQ_HZ  915000000L   // AU/NZ
#define PROTO_LORA_SF       9
#define PROTO_LORA_BW_HZ    125000L
#define PROTO_LORA_CR       5            // 4/5

// ── Device IDs ──────────────────────────────────────────────────────────────
#define DEVICE_ID_PUMP      0x01
#define DEVICE_ID_IRRIGATOR 0x02

// ── Message types ───────────────────────────────────────────────────────────
#define MSG_PUMP_ON         0x10
#define MSG_PUMP_OFF        0x11
#define MSG_PUMP_STATE      0x12   // pump -> beacon reply, payload: 1 = pumping
#define MSG_HEARTBEAT       0x20
#define MSG_GPS_POSITION    0x30
#define MSG_ALERT_STALL     0x40
#define MSG_PUMP_CUTOFF     0x50   // logged by the Pi as a KICKOUT

// Heartbeat payload byte 0 flags (byte 1 = battery %)
#define HB_FLAG_PUMP_ON     0x01
#define HB_FLAG_NO_FIX      0x02

#define GPS_PACKET_LEN      14     // 2 header + 12 payload
#define POS_PACKET_LEN      10     // 2 header + lat + lon (alerts)

struct GpsReport {
  double  lat, lon;
  int16_t speedCms;
  uint8_t battPct;
  bool    pumpOn;
};

inline void protoPutI32(uint8_t *b, int32_t v) {
  b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
}
inline int32_t protoGetI32(const uint8_t *b) {
  return (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8)  |  (uint32_t)b[3]);
}
inline int32_t protoDegToI32(double deg) {
  return (int32_t)(deg * 1e6 + (deg >= 0 ? 0.5 : -0.5));
}

// Build a full MSG_GPS_POSITION packet. buf must hold GPS_PACKET_LEN bytes.
inline int protoEncodeGps(uint8_t *buf, uint8_t src, const GpsReport &r) {
  buf[0] = src;
  buf[1] = MSG_GPS_POSITION;
  protoPutI32(buf + 2, protoDegToI32(r.lat));
  protoPutI32(buf + 6, protoDegToI32(r.lon));
  buf[10] = (uint8_t)((uint16_t)r.speedCms >> 8);
  buf[11] = (uint8_t)r.speedCms;
  buf[12] = r.battPct;
  buf[13] = r.pumpOn ? 1 : 0;
  return GPS_PACKET_LEN;
}

// Decode the payload of a MSG_GPS_POSITION packet (bytes after src + type).
inline bool protoDecodeGps(const uint8_t *payload, int len, GpsReport &r) {
  if (len < 12) return false;
  r.lat      = protoGetI32(payload)     / 1e6;
  r.lon      = protoGetI32(payload + 4) / 1e6;
  r.speedCms = (int16_t)(((uint16_t)payload[8] << 8) | payload[9]);
  r.battPct  = payload[10];
  r.pumpOn   = payload[11] != 0;
  return true;
}

// Build an alert-style packet (MSG_ALERT_STALL / MSG_PUMP_CUTOFF) carrying a
// position. buf must hold POS_PACKET_LEN bytes.
inline int protoEncodePos(uint8_t *buf, uint8_t src, uint8_t type,
                          double lat, double lon) {
  buf[0] = src;
  buf[1] = type;
  protoPutI32(buf + 2, protoDegToI32(lat));
  protoPutI32(buf + 6, protoDegToI32(lon));
  return POS_PACKET_LEN;
}
