# Changelog

Running log of infra/schema changes on the shared Pi server (192.168.5.111). Multiple
Claude Code sessions and people work on this repo — if your change touches a database
schema, a running service's config, or anything another session might collide with,
add an entry here (newest on top) so the next session doesn't have to reverse-engineer
it from `git log`.

Include: what changed, why, any manual step needed on the Pi beyond a normal
`git pull` + service restart, and anything another session should know before touching
the same files.

---

## 2026-08-19 — EM300-DI water meter support

**What:** Added support for Milesight EM300-DI LoRaWAN pulse counters as the new water
metering hardware (see `water-metering-files/EM300-DI-SETUP.md` for device config —
units have been ordered).

**Files touched:** `water-metering-files/server.js`, `water-metering-files/public/index.html`.

**Schema changes (auto-applied on next `node server.js` start via the existing
PRAGMA-check-then-ALTER migration pattern — no manual SQL needed):**
- `water_readings` gains a `source` column (`'legacy'` default, `'em300-di'` for new
  readings) so mixed-protocol rows are distinguishable when browsing the DB.
- `devices` gains a `meter_role` column (`'tank'` / `'farm'` / `NULL`) — set from the
  Device Settings tab, tells the EM300-DI decoder which meter a given unit reads.
- New table `water_meter_counters` — persists each EM300-DI's last cumulative pulse
  count across server restarts (the device reports a running total, not a delta;
  restarting without this would double-count or reset to a huge spurious delta).

**Behavior change:** the existing custom-firmware protocol on **fPort 2**
(`tank_pulses`/`farm_pulses` in one packet) is untouched and still works as before.
EM300-DI units use **fPort 85** (Milesight's default) and are handled by a new,
separate branch in the same MQTT handler — the two protocols coexist.

**If you're touching `water-metering-files/server.js` or the Device Settings UI
concurrently:** the new code lives in the `mqttClient.on('message', ...)` handler
(new `if (fPort === 85)` block, after the existing `fPort === 2` block) and in the
`/api/devices` POST/PUT handlers (`meter_role` field). Check `git log -p` on those
before assuming a merge conflict is trivial.

**Not yet done:** no physical EM300-DI has been registered in Device Settings yet —
that happens per-unit as they're commissioned, see the setup doc.
