# Irrigator GPS beacon node — design

Date: 2026-09-19
Scope: `irrigator-system` — the field unit on the travelling effluent irrigator,
plus the pump-module change that goes with it.

## Purpose

Report the irrigator's position over LoRa for the spread map, and stop the pump
if the irrigator stops travelling while pumping. Solar powered, no wifi or RTK
at the site, and the unit is routinely sprayed with effluent.

## Key facts and decisions

| Item | Decision |
|---|---|
| Travel speed | ~100 m/h (≈8.3 m per 5 min) |
| Positioning | Standalone GNSS, no RTK. 1–1.5 m is ample for a 24 m swath and for stall detection at this speed |
| GNSS antenna | Spare drone helical, 80 mm high, SMA female. No ground plane needed |
| MCU / radio | LilyGO T3 (ESP32 + SX1276), already owned. `LoRa.h`, 915 MHz, SF9, BW125, CR4/5 — unchanged |
| GNSS receiver | u-blox M10 breakout on UART, with switched supply and V_BCKP kept alive for hot starts |
| Stall decision | Moves from the field node to the pump module (mains powered, knows pump state) |
| Silence trip | None. Loss of beacon does not stop the pump; the Pi shows "irrigator not heard" |
| Battery | 2–3 × 18650 in parallel with protection. SLA not needed |
| Panel | 10 W 6 V, mounted flat |
| Enclosure | Printed ASA sealed box, panel on top of lid, pitched clear PC shroud over |

Superseded: `irrigator-module` (T-Beam v1.1) and `irrigator-module-s3` remain in
the repo for reference but are not the deployed design. Their always-listening
architecture costs ~8 Wh/day; the beacon costs ≤2 Wh/day.

## 1. Field beacon firmware — `irrigator-system/irrigator-beacon-t3/`

Cycle (deep sleep between cycles):

1. Wake on timer. Power GNSS on if it is off (GPIO → module EN or P-FET).
2. Wait for a fresh valid fix, timeout 60 s.
3. Transmit the existing 12-byte `MSG_GPS_POSITION` packet
   (src `0x02`; lat, lon ×1e6 int32; speed cm/s int16; battery %; pump_on).
   - `pump_on` is the last state learned from the pump module's reply.
   - Speed is derived from the previous fix held in RTC memory, not Doppler
     (Doppler noise exceeds 0.03 m/s travel speed).
4. Open a 600 ms RX window for `MSG_PUMP_STATE` (new, `0x12`, src `0x01`,
   payload 1 byte: 1 = pumping). The pump module replies ~40 ms after
   receiving, so the beacon is already listening.
5. Deep sleep.
   - Pump on: 30 s interval, and the GNSS stays powered through the sleep
     (pin held) so it tracks continuously — hot-start first fixes are noisier
     and would degrade the stall check.
   - Pump off or unknown: 5 min interval, GNSS main rail off between wakes.
   - Three consecutive missed replies while believed pumping → assume pump
     off. The next heard reply restores 30 s.

No fix within 60 s: send `MSG_HEARTBEAT` with payload bit 1 = "no fix" so the
link is still seen alive, then sleep as normal.

Battery %: T3 battery divider on GPIO35, averaged, mapped from a Li-ion
voltage curve (3.3 V = 0 %, 4.15 V = 100 %).

State kept in RTC memory across deep sleep: last fix, last pump state, missed
reply count.

## 2. Pump module changes — `irrigator-system/pump-module/`

- On every `MSG_GPS_POSITION` from `0x02`: reply immediately with
  `MSG_PUMP_STATE`, and push the position into a ring buffer.
- Stall logic lives in a plain C++ header `stall_detector.h` with no Arduino
  dependencies:
  - Input: `(lat, lon, t_ms)` samples and pump state.
  - Armed only after the pump has been on for `STALL_GRACE_MS` (15 min).
  - Stalled when the mean of the newest 3 samples is < `STALL_THRESH_M` (3 m)
    from the mean of 3 samples taken ≥ `STALL_WINDOW_MS` (5 min) earlier, on
    2 consecutive samples. Averaging is needed: single fixes carry ~1.5 m of
    noise, which would false-trip a 3 m test a few times per 12 h run.
  - After a genuine stop the verdict arrives ~3.5 min later (the window still
    holds travel until then).
  - Newest sample older than 2 min → no verdict (no silence trip).
  - Buffer is cleared on pump-off, so each pump run starts fresh.
  - Insufficient samples in the window (lost packets) → not stalled.
- On stall: broadcast `MSG_ALERT_STALL`, call the existing `pulseCutoff()`
  once (existing `cutLatched` behaviour — re-arms on manual restart), then
  broadcast `MSG_PUMP_CUTOFF`; both from src `0x01` with the irrigator's last
  position. The Pi already logs these as STALL and KICKOUT. If the pump is
  still sensed running, `MSG_ALERT_STALL` repeats every 1 min.
- `MSG_PUMP_CUTOFF` handling from `0x02` is retained so the older modules still
  work.

## 3. Bridge and Pi server

No changes. `lora_bridge.ino` forwards alert types from any source and
silently ignores unknown types such as `MSG_PUMP_STATE`; `receiver.py` already
maps the two alert types to STALL and KICKOUT. Confirmed in the bench test.

## 4. Power

Panel 10 W 6 V → CN3791 MPPT charger module (set for 1-cell Li-ion, 6 V panel)
→ 2–3 × 18650 parallel pack with protection → T3 battery JST.

Thermistor charge cutoff: NTC on the pack gating the CN3791 so charging stops
below ~0 °C and above ~45 °C.

Budget, worst case pumping 24 h/day: GNSS tracking continuously, ~25 mA
average ≈ 2.2 Wh/day. Pump off:
well under 1 mA average. Flat panel, NZ winter, shroud and dried-effluent
losses all applied: ~3–4 Wh/day. Two cells (~25 Wh) give >10 days with no sun.

Check on the bench: T3 deep-sleep current with GNSS main rail off. If it is
above ~2 mA (OLED or regulator leakage), deal with it before field use.

Check when buying: the helical is almost certainly an active antenna. The M10
breakout must supply antenna bias (3–5 V) on its RF connector.

## 5. Enclosure (CAD is a separate plan, after electronics are bench-proven)

- **Box:** one-piece ASA tub, printed floor-down, walls ≥3 mm / ≥5 perimeters,
  interior sealed with ASA slurry or epoxy. Flat opaque ASA lid. Rim groove
  with 3 mm closed-cell EPDM or silicone cord (no printed TPU gasket). Stainless
  M4 screws outside the seal line into blind heat-set inserts. Membrane vent in
  the floor facing down.
- **Panel:** flat on top of the lid in a shallow recess with retaining clips.
  Lead enters through a potted gland in the lid beneath the panel.
- **Helical:** stands on the lid at one end, beside the panel (not under it —
  the cells block GNSS). Lid feedthrough is an O-ring-sealed SMA bulkhead
  presenting SMA male upward for the antenna's SMA female; pigtail to the M10
  inside. Lid is ~50 mm longer than the panel to make room. The antenna's
  magnetic base is not used on the box (nothing steel to hold it, and its
  cable would need a gland) — it is handy for bench and range testing.
- **LoRa antenna:** whip inside the box along a wall (ASA is transparent at
  915 MHz). Fallback if range is short: a second sealed SMA bulkhead with the
  whip under the shroud.
- **Shroud:** 2–3 mm UV-stabilised polycarbonate, cold-bent to a gable with
  15–20° pitch each side, ridge along the long axis. Printed ASA gable end caps
  with slots to take the sheet. Clearance under the ridge at the antenna
  ≥ 95 mm above the lid (80 mm antenna + connector + margin). Eaves overhang
  the box walls and drop below the lid joint as a skirt, with a small air gap
  so the space under the shroud ventilates. The box is sealed without relying
  on the shroud.
- **Mount:** level bracket to the irrigator frame; details when the mounting
  spot is chosen.
- Sizes come from the measured panel; must fit the QIDI Max 4 bed.
- CAD in CadQuery, following `rtk-rover-enclosure/` conventions.

## 6. Testing

1. **Host unit tests** for `stall_detector.h` (compiled with g++ on the PC)
   using synthetic tracks:
   - 100 m/h travel with 1.5 m noise → never stalled.
   - Stationary with 1.5 m noise, pump on > grace → stalled within
     window + one sample.
   - Travel then stop → stalled ~5 min after stopping.
   - Pump on < 15 min → never stalled.
   - Sparse packets (every third lost) → same outcomes.
   - Pump off/on → buffer reset, grace restarts.
2. **Bench:** T3 beacon + pump module + bridge on the desk. Confirm the
   packet/reply exchange, interval switching on pump state, relay pulse on a
   simulated stall (static beacon, shortened grace), alert reaching the Pi.
   Measure sleep and average current.
3. **Field:** range check beacon → shed with the whip inside the box; one full
   irrigator run logged on the Pi; tune `STALL_THRESH_M` / `STALL_WINDOW_MS`
   from the recorded track noise.

## Build order

1. Plan A — firmware and bench electronics (sections 1–4, 6).
2. Plan B — enclosure CAD and print (section 5), once the panel is in hand.

## Out of scope

RTK, rotation sensor, silence trip, changes to the Pi database or map UI,
LoRaWAN.
