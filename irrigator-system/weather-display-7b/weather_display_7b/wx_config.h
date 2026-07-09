#pragma once

// ── WiFi ─────────────────────────────────────────────────────────────────
#define WX_WIFI_SSID      "YOUR_WIFI_SSID"
#define WX_WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// ── Pi weather backend ──────────────────────────────────────────────────
// Flask has no auth on /weather/* (only the Node web UI proxy does), so the
// display talks to Flask directly on port 5000.
//
// Same LAN as the Pi: use its LAN IP, e.g. "192.168.5.111"
// Remote over Tailscale: use the Pi's Tailscale IP. This ESP32 board can't
// run a Tailscale client itself, so it only works if the display's own
// network can already route to that IP (e.g. it's on a Tailscale subnet
// router / exit node, or a travel router running Tailscale). Otherwise keep
// the display on the Pi's local LAN.
#define WX_SERVER_HOST    "192.168.5.111"
#define WX_SERVER_PORT    5000

// History period fetched for the tap-to-expand charts: "24h", "7d", or "30d"
#define WX_CHART_PERIOD   "24h"

// How often to poll for new data, in milliseconds
#define WX_POLL_MS        60000UL
