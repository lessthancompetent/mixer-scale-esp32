# Irrigator beacon (LilyGO T3 + u-blox M10)

Field unit on the travelling irrigator. Each wake it gets a GNSS fix, sends it
over LoRa, listens 600 ms for the pump module's `MSG_PUMP_STATE` reply, then
deep-sleeps: 30 s between wakes while the pump runs, 5 min otherwise. The
**pump module** decides when the irrigator has stalled — this node only reports.

Design: `docs/superpowers/specs/2026-09-19-irrigator-beacon-node-design.md`

## Wiring

| T3 pin | Goes to | Notes |
|---|---|---|
| GPIO34 | GNSS TX | input-only pin, fine for RX |
| GPIO4 | GNSS RX | |
| GPIO13 | GNSS EN (or P-FET gate driver) | 100k pull-down to GND so the GNSS is off during reset |
| 3V3 | GNSS VCC | via the P-FET if the module has no EN pin |
| GND | GNSS GND | |
| BAT JST | 18650 pack (protected, 2–3P) | pack is charged by the CN3791 from the 6 V panel |

- GNSS backup supply (V_BCKP / on-board cell) must stay powered so wakes are
  hot starts.
- The drone helical is an active antenna: the GNSS module must put 3–5 V bias
  on its RF connector.
- Pins live in `board_t3.h`. `LORA_RST` is 23 on a T3 v1.6.1, 14 on a v1.0 —
  "[LoRa] init failed" on the serial monitor means try the other one.
- `GPS_BAUD` is 38400 (u-blox M10 default). If no fix ever arrives and the
  module is a 9600 baud variant, change it there.

## Build and flash

```bash
pio run -d irrigator-system/irrigator-beacon-t3 -t upload
pio device monitor -d irrigator-system/irrigator-beacon-t3
```

Expected serial output each wake:

```
[TX] -45.500123, 168.300456  sats=14  batt=87%
[SLEEP] pump=0 missed=1  300s
```

`missed` counts wakes with no reply from the pump module; three in a row while
pumping drops the beacon back to the 5 min interval.

## Host tests

The decision logic is in `../common` and is tested on the PC:

```bash
PATH="/c/msys64/ucrt64/bin:$PATH" sh irrigator-system/common/test/run_tests.sh
```
