# Milesight EM300-DI Setup — Water Meter Pulse Counters

Two units needed: one reading the **tank** meter's reed switch, one reading the
**farm** meter's reed switch. Each EM300-DI has only one pulse input, so it can't
report both like the old custom node did — that's why the server-side code tracks
a `meter_role` per device instead of assuming one node = both meters.

Payload format, fPort, and wiring were confirmed against Milesight's own decoder repo
and datasheet search results — see sources at the bottom. The one thing **not**
independently verified against a physical unit or the full PDF user guide (several
Milesight documentation domains aren't reachable from where this was researched) is
the exact ToolBox app screen wording in step 2 — the settings themselves (GPIO mode,
port, interval) are confirmed from the device's own downlink command set, just not
the exact menu labels.

## 1. Wiring

- The GPIO lead is a **fixed 1 m cable (3.5mm diameter)** exiting the IP67 enclosure
  through a sealed cable gland — there's no user-openable terminal block on the
  device. Two color-coded bare wires at the end: **red = GPIO/signal, black = GND**.
- Splice (or wire-nut) those two leads directly onto the reed switch meter's own two
  leads. No polarity — it's a passive dry-contact loop.
- For the run from the EM300-DI to wherever the meter physically sits: use
  **shielded twisted-pair cable** (22–24 AWG, like a 4-20mA loop or RS-485 stub),
  shield grounded at the EM300-DI end only. This is a slow signal (real water meter
  pulses are ≤1/sec, well under the device's 2000 Hz / 250 µs limits), so simple wire
  resistance isn't the constraint — noise pickup from nearby pumps/motors is, and
  shielded pair handles farm-length runs (well past 50–100 m) fine where bare
  unshielded wire wouldn't.
- Protect the splice point — it's the one place that isn't factory-sealed. A sealed
  junction box or heat-shrink + gel-filled connectors, not open wire nuts, if it's
  going to sit outside.

## 2. Device configuration (before deploying, via Milesight ToolBox app — NFC, Android/iOS)

Do this **before** mounting the unit somewhere hard to reach — it's a local NFC
tap-to-configure step, no network join needed yet.

1. **Set GPIO mode to Counter, not Digital Input.** This is the critical step — out
   of the box the device may report raw GPIO high/low state instead of a pulse
   count, and the server code only understands the pulse-count channel. Look for a
   "Work Mode" or "GPIO Mode" setting on the digital input screen; set it to
   **Counter / Pulse Counter** (not "Digital Input").
2. Confirm the **application port (fPort) is 85** (factory default — shouldn't need
   changing, but confirm rather than assume).
3. Set the **report interval** — how often it uplinks. Every 10–15 min is a
   reasonable default matching the other field devices on this network; shorter
   intervals cost battery.
4. Confirm the **frequency plan / region matches your ChirpStack setup** (e.g. AU915
   if that's what the other devices here use).
5. Note the printed **DevEUI** and **AppKey** on the device label/box — you'll need
   both for ChirpStack and for the web UI registration below.

## 3. Register in ChirpStack

1. Add the device to the existing Application (same one the other field devices use)
   as an **OTAA** device, using the DevEUI/AppKey from the label.
2. Leave the device profile's payload codec as **None/Raw** — decoding happens in
   `server.js`, not in ChirpStack, matching how the other devices on this network
   (irrigator, weather station, feed mixer) already work.
3. Confirm it joins (check ChirpStack's device event log) once powered on near a
   gateway.

## 4. Register in the web UI

Web UI → **Device Settings** tab (password protected) → **+ Add Device**:

| Field | Value |
|---|---|
| Name / Label | e.g. "Tank Water Meter (EM300-DI)" |
| Type | **Water Meter** |
| Meter Role | **Tank** or **Farm** — this appears once Type is set to Water |
| DevEUI | from the device label |
| AppEUI / JoinEUI | from ChirpStack / the device label |
| AppKey | from the device label |

Repeat for the second unit with the other role. **Without this step the server logs
a warning and drops the uplink** — `[WATER-EM300DI ...] uplink from
unregistered/unroled device`, visible via `journalctl -u water-metering.service -f`
(or whatever the service is named on this Pi) — so if a newly deployed unit's data
isn't showing up, check there first before assuming a wiring or join problem.

## 5. Verify

Watch the service log after the unit's first couple of uplinks:

```
[WATER-EM300DI a1b2c3...] first uplink seen, storing baseline pulse=1042
[WATER-EM300DI a1b2c3...] tank+3 pulses (battery 100%)
```

The first uplink after registering always just stores a baseline (no delta yet —
there's nothing to diff against). From the second uplink on you should see
`tank+N` or `farm+N` matching however many pulses the meter turned since the last
report. Cross-check against the Water Meters tab in the web UI.

## Sources

- [Milesight EM300-DI decoder & payload spec](https://github.com/Milesight-IoT/SensorDecoders/tree/main/em-series/em300-di)
- [Milesight EM300-DI LoRaWAN device profile](https://github.com/Milesight-IoT/lorawan-devices/blob/master/vendor/milesight-iot/em300-di.yaml)
- [EM300 Series Sensor FAQ](https://support.milesight-iot.com/support/solutions/articles/73000600419-em300-series-sensor-faq)
