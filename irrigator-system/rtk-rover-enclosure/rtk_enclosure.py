#!/usr/bin/env python3
"""
Parametric, water-resistant enclosure for a portable RTK GNSS rover built
around the WildBuckwheat "simpleRTK2B Micro breakout board" v2.1 (ArduSimple
simpleRTK2B Micro / u-blox ZED-F9P plugged into it, HC-05 Bluetooth on the
back), an internal 18650 cell in a holder, a charger/boost module, a sealed
power switch, a capped charging connector and an SMA bulkhead for the
antenna pigtail.

Sealing concept (like a commercial IP65 box):
  * a 2.0 mm silicone O-ring cord sits in a groove around the top of the
    base wall and is squeezed 25 % by the flat underside of the lid,
  * the six lid screws sit in pillars inside the smooth shell; the gasket
    passes on the cavity side of them, so they stay outside the seal,
  * every wall opening is a round hole for a gasketed panel component
    (SMA bulkhead, booted toggle switch, capped GX12/DC charging jack).
  * the breakout's own micro-USB has no opening by default (usb_mode="none").

All board geometry below was taken from the breakout's published gerbers
(Gerber_BoardOutlineLayer.GKO, Drill_PTH_Through.DRL, pick-and-place CSV,
2023-11-26, https://github.com/WildBuckwheat/SimpleRTK2B-Micro-breakout-board):

    outline  : 31.90 x 38.25 mm, corner radius 3.25 mm
    holes    : 4 x 3.50 mm on a 25.40 x 31.75 mm pattern; the hole centres
               are the corner-arc centres (3.25 mm in from each edge)
    micro USB: SMD Micro-B on the +Y edge (the edge the "ANT" arrow points
               to), plug axis perpendicular to that edge
    LEDs     : PWR (green) and RTK (red) 0603 on the top face next to USB

Run:   python3 rtk_enclosure.py      -> out/*.stl, out/*.step, out/*.png
                                        and a printed layout + fit report
Needs: pip install cadquery          (matplotlib comes with it)

Enclosure coordinate system (mm):
    X : long axis. +X end wall carries the SMA bulkhead (and the optional
        micro-USB opening); -X end wall carries the charge jack and switch.
    Y : across. Battery bay along the -Y wall, board row along +Y.
    Z : up. Z = 0 is the top surface of the floor inside the base.
"""
import os
import sys

import cadquery as cq

# --------------------------------------------------------------------------
# Parameters (mm).  Change, re-run, re-slice.
# --------------------------------------------------------------------------
P = dict(
    # ---- shell -------------------------------------------------------------
    wall=4.0,            # side wall thickness (holds the 2.2 mm gasket groove)
    floor=2.4,           # base floor thickness
    lid_t=3.2,           # lid plate thickness
    corner_r=3.0,        # inside vertical corner radius of the cavity
    outer_r=7.0,         # outside vertical corner radius
    edge_r=1.5,          # fillet on the outside bottom edge of the base and top edge of the lid
    inner_h=24.0,        # floor top to lid underside (24 clears an SMA plug on the Micro; 22 is enough for U.FL)

    # ---- gasket ------------------------------------------------------------
    cord_d=2.0,          # silicone O-ring cord diameter
    groove_w=2.2,        # groove width
    groove_depth=1.5,    # groove depth -> 25 % squeeze on a 2.0 mm cord
    groove_offset=2.0,   # groove centre line distance from the cavity wall

    # ---- lid screws (M3 thread-forming into the lugs) -----------------------
    lug_r=6.0,           # screw pillar radius (pillars are inside the smooth outer shell)
    lug_inset=5.5,       # screw centre distance in from the outside faces
    pillar_blend=3.0,    # blend radius where a pillar meets the cavity wall
    lug_hole=2.5,        # pilot hole (use 4.0 for an M3 heat-set insert)
    lug_hole_depth=14.0,
    lid_screw_d=3.4,
    lid_cbore_d=6.4, lid_cbore_depth=1.5,
    mid_lugs=True,       # extra pillar mid-length on each long wall (6 screws)
    lip_t=1.2, lip_h=2.0, lip_clear=0.3,   # lid locating lip inside the cavity

    # ---- pipe clamp on the back (floor) face ---------------------------------
    # Four blind M3 thread-forming holes go up into bosses beside the long walls,
    # so nothing passes through the sealed floor. pipe_od: NZ/AU "20 mm" PVC
    # pressure pipe is DN20 = 26.7 mm OD; 20 mm conduit is 20.0 mm OD.
    fix_holes=True,
    fix_dx=15.0,         # hole offset along X either side of the box centre
    fix_wall_gap=3.5,    # hole centre distance from the inside of the long wall
    fix_boss_d=7.0, fix_boss_h=6.0, fix_hole=2.5, fix_hole_cap=1.2,
    pipe_od=26.7,
    pipe_variants=(20.0, 26.7),   # clamp STLs are written for each of these
    clamp_len=40.0, clamp_plate_t=6.0, clamp_min_t=4.0, clamp_ring_t=4.0, clamp_gap=2.0,
    clamp_bolt_d=4.4, clamp_bolt_dx=12.0, clamp_bolt_off=8.5, clamp_flange=14.0,
    clamp_nut_af=7.2, clamp_nut_seat=8.0,        # M4 nut pocket; seat height above the parting face
    clamp_screw_d=3.4, clamp_cbore_d=6.2, clamp_cbore_depth=3.0,

    # ---- antenna mount for the top of the pipe (u-blox ANN-MB on a metal ground plane) ----
    ant_gp_d=120.0,      # metal ground-plane disc; u-blox characterise the ANN-MB on 120-150 mm
    ant_gp_t=1.5,        # disc thickness (steel, so the antenna's magnets hold it)
    ant_tray_t=3.0,      # printed tray under the disc
    ant_sleeve_len=35.0, ant_sleeve_wall=4.0,
    ant_slot=3.0,        # pinch slot in the sleeve
    ant_ear_w=6.0, ant_ear_len=13.0, ant_ear_h=16.0,   # pinch-clamp ears, one M4 x 25 + nut
    ant_ribs=5, ant_rib_t=3.0,
    ant_disc_screw_r=50.0, ant_disc_screw_d=2.5,       # 3 x M3 self-tappers through the disc (optional)
    ant_size=(82.0, 60.0, 22.5),                        # ANN-MB body, for the mock-up
    antenna="ann_mb",    # which antenna the assembly/renders show: "ann_mb", "helical" or "survey"
    # survey antenna (K700 type) adapter: a real 5/8"-11 UNC hex bolt, head trapped between the
    # cap and the pipe end, thread standing up through the top. Use a 3/4" long bolt (~14 mm proud).
    srv_bolt_d=16.3, srv_hex_af=24.2, srv_hex_h=10.4, srv_top_t=5.0, srv_body_r_min=18.5,
    srv_size=(150.0, 62.0),                             # survey antenna dia x height, for the mock-up
    # helical antenna cap: SMA bulkhead (female-female, or the bulkhead end of an extension lead)
    # through the top, cable leaves by a side window just above the pipe end
    hel_top_t=4.0, hel_sma_d=6.5, hel_chamber_h=18.0, hel_window_w=11.0, hel_window_h=13.0,
    hel_size=(28.0, 60.0),                              # helical antenna dia x height, for the mock-up

    # ---- breakout board (from gerbers) ---------------------------------------
    pcb_len=38.25,       # along enclosure X (gerber Y)
    pcb_w=31.90,         # along enclosure Y (gerber X)
    pcb_t=1.6,
    pcb_corner_r=3.25,
    hole_d=3.5,
    pcb_end_gap=2.0,     # board +X edge to the inner face of the +X wall
    pcb_side_clear=1.0,  # row width margin each side of the board

    # ---- board mounting ------------------------------------------------------
    standoff_h=10.0,     # floor top to board underside (HC-05 lives here)
    standoff_d=6.0,
    standoff_hole=2.5,   # M3 thread-forming, 7 mm deep
    standoff_hole_depth=7.0,
    standoff_flare_d=8.5, standoff_flare_h=2.0,

    # ---- things on the board (keep-outs for the fit check) -------------------
    micro_w=24.0,        # simpleRTK2B Micro across the socket rows (+1 mm each side)
    micro_len=31.0,      # along the socket rows incl. antenna connector
    micro_h=9.5,         # above breakout top face incl. sockets and U.FL
    # Antenna connector on the Micro. "sma": edge-mount SMA. The breakout is then
    # turned 180 deg so the jack points over the low charger module (there is no
    # room for it against the +X wall), and the bulkhead moves up beside the board
    # so the pigtail is a straight run. "ufl": board as before, bulkhead at the
    # end of the battery row.
    micro_ant="sma",
    micro_sma_len=22.0,  # jack + right-angle plug beyond the Micro's edge (keep-out)
    micro_sma_d=11.0,    # keep-out height/width around the jack axis
    hc05_w=16.5, hc05_h=8.5,   # HC-05 (ZS-040 carrier) under the board

    # ---- 18650 holder bay (generic single 18650 holder with wire leads) -------
    batt_len=78.5,       # holder 77.5 + 1 clearance
    batt_w=21.5,         # holder 20.5 + 1 clearance
    batt_h=15.0,         # holder height, information only
    batt_rib_h=3.0, batt_rib_t=1.5,
    batt_screw_pitch=0,  # 0 = none; else two M3 holes in the floor on this pitch (breaks the seal)
    sma_bay_len=16.0,    # free length at the +X end of the battery row for the SMA jack

    # ---- charger / boost module bay ------------------------------------------
    # default fits Adafruit PowerBoost 1000C (36.3 x 22.9); TP4056+boost boards fit too
    mod_len=37.5, mod_w=24.0, mod_h=7.0,
    mod_rib_h=1.5, mod_rib_t=1.2,
    mod_y_shift=1.5,     # module bay centre relative to the board-row centre (wire lane by the divider)

    # ---- -X end wall: charge jack and power switch ---------------------------
    charge_d=12.2,       # GX12 aviation connector or IP67 5.5x2.1 DC jack (12 mm panel hole)
    charge_z=11.0,
    charge_body_d=13.0, charge_body_depth=16.0,   # keep-out behind the wall
    sw_d=6.2,            # MTS-102 mini toggle with silicone boot (6 mm bushing)
    sw_z=11.0,
    sw_body_w=13.5, sw_body_h=13.5, sw_body_depth=16.0,   # keep-out incl. terminals

    # ---- +X end wall: SMA bulkhead and optional micro-USB opening -------------
    sma_d=6.5,           # 1/4-36 SMA bulkhead
    sma_flat=0.0,        # e.g. 5.8 for a D-shaped anti-rotation hole, 0 = round
    sma_z=11.0,          # micro_ant="ufl"
    sma_z_high=17.0,     # micro_ant="sma": bulkhead above the breakout's corner, in line with the cable
    sma_body_len=15.0,   # keep-out behind the wall (jack body + crimp)
    usb_mode="none",     # "none" (sealed), "direct" (hole for a plug), "panel" (panel-mount extension)
    usb_cut_w=13.0, usb_cut_h=8.5, usb_cut_r=2.0,
    usb_center_above_pcb=1.3,
    usb_panel_screw_pitch=20.0, usb_panel_screw_d=2.6,

    # ---- optional pressure-equalising vent, -X wall above the battery bay ------
    vent_d=0.0,          # e.g. 6.4 for an M6 Gore-type vent, 0 = none
    vent_z=17.5,

    # ---- LED windows in the lid above PWR/RTK ---------------------------------
    led_window="thin",   # "thin" (0.8 mm skin, back-fill with clear resin), "hole", "none"
    led_window_d=4.0, led_skin=0.8, led_hole_d=2.0,

    out_dir="out",
    stl_tol=0.05,
)

# Board features in the gerber frame (mm, origin = J1 pin 1)
GERBER = dict(
    x_min=-1.980, x_max=29.920, y_min=-7.710, y_max=30.540,
    holes=[(1.27, -4.46), (26.67, -4.46), (1.27, 27.29), (26.67, 27.29)],
    usb=(19.304, 27.178),            # USB1 mid point, plug enters from +Y
    usb_face_y=31.6,                 # receptacle face, ~1 mm past the edge
    leds=[(11.176, 28.321), (12.70, 28.321)],   # PWR, RTK
    micro_rows=(2.971, 24.970),      # H1/H2 2 mm-pitch socket rows
    micro_y=(-1.0, 30.0),            # Micro board extent along gerber Y (est.)
    hc05_y=(-5.5, 32.0),             # HC-05 carrier extent along gerber Y (est.)
)


def gxy(L, gx, gy):
    """Breakout (gerber) coordinates -> enclosure X, Y."""
    return (L["bx0"] + L["bs"] * gy, L["by0"] - L["bs"] * gx)


def gspan(L, gx0, gx1, gy0, gy1):
    (xa, ya), (xb, yb) = gxy(L, gx0, gy0), gxy(L, gx1, gy1)
    return (min(xa, xb), min(ya, yb), max(xa, xb), max(ya, yb))


def gbox(L, gx0, gx1, gy0, gy1, z0, z1, r=0.0):
    """Box given in breakout (gerber) x/y and enclosure Z."""
    x0, y0, x1, y1 = gspan(L, gx0, gx1, gy0, gy1)
    return rbox(x0, y0, z0, x1, y1, z1, r)


# --------------------------------------------------------------------------
# Derived layout
# --------------------------------------------------------------------------
def layout(P):
    L = {}
    # the screw pillars stand inside the cavity; keep the component rows clear of them
    ym = P["lug_inset"] - P["wall"] + P["lug_r"] + 0.5
    L["y_margin"] = ym
    batt_x0 = 4.0
    row_batt = (ym, ym + P["batt_w"])
    div = (row_batt[1], row_batt[1] + P["batt_rib_t"])
    row_w = P["pcb_w"] + 2 * P["pcb_side_clear"]
    row_pcb = (div[1], div[1] + row_w)
    L["W_in"] = row_pcb[1] + ym
    L["batt"] = (batt_x0, row_batt[0], batt_x0 + P["batt_len"], row_batt[1])
    L["div"] = div
    L["L_in"] = L["batt"][2] + P["sma_bay_len"]
    L["H_in"] = P["inner_h"]
    L["row_pcb"] = row_pcb
    L["row_c"] = 0.5 * (row_pcb[0] + row_pcb[1])

    # board frame -> enclosure frame: X = bx0 + bs * gy ; Y = by0 - bs * gx   (bs = -1: board turned 180 deg)
    g = GERBER
    flip = P["micro_ant"] == "sma"
    if flip and P["usb_mode"] != "none":
        raise ValueError('micro_ant="sma" turns the breakout USB inwards; usb_mode must be "none"')
    bs = L["bs"] = -1.0 if flip else 1.0
    pcb_xmax = L["L_in"] - P["pcb_end_gap"]
    L["bx0"] = pcb_xmax - bs * (g["y_min"] if flip else g["y_max"])
    L["by0"] = L["row_c"] + bs * 0.5 * (g["x_min"] + g["x_max"])
    L["pcb_box"] = gspan(L, g["x_min"], g["x_max"], g["y_min"], g["y_max"])
    L["holes"] = [gxy(L, gx, gy) for gx, gy in g["holes"]]
    L["usb_y"] = gxy(L, *g["usb"])[1]
    L["usb_z"] = P["standoff_h"] + P["pcb_t"] + P["usb_center_above_pcb"]
    L["usb_face_x"] = gxy(L, 0, g["usb_face_y"])[0]
    L["leds"] = [gxy(L, gx, gy) for gx, gy in g["leds"]]

    # -X end wall: charge jack low in the row, switch high in the row
    L["charge_y"] = row_pcb[0] + 0.5 + P["charge_body_d"] / 2
    L["sw_y"] = row_pcb[1] - P["corner_r"] - P["sw_body_w"] / 2
    # module bay behind them, centred (shifted) in the board row
    mod_x0 = max(P["charge_body_depth"], P["sw_body_depth"]) + 2.0
    mc = L["row_c"] + P["mod_y_shift"]
    L["mod"] = (mod_x0, mc - P["mod_w"] / 2, mod_x0 + P["mod_len"], mc + P["mod_w"] / 2)
    # SMA in the +X wall in front of the battery row
    if flip:
        L["sma_y"], L["sma_z"] = 0.5 * (div[0] + div[1]) + 0.25, P["sma_z_high"]
    else:
        L["sma_y"], L["sma_z"] = 0.5 * (row_batt[0] + row_batt[1]) + 1.0, P["sma_z"]
    L["vent_y"] = 0.5 * (row_batt[0] + row_batt[1])

    e = P["lug_inset"] - P["wall"]
    lugs = [(e, e), (L["L_in"] - e, e), (e, L["W_in"] - e), (L["L_in"] - e, L["W_in"] - e)]
    if P["mid_lugs"]:
        lugs += [(L["L_in"] / 2, e), (L["L_in"] / 2, L["W_in"] - e)]
    L["lugs"] = lugs
    g_ = P["fix_wall_gap"]
    L["fix"] = [(L["L_in"] / 2 + sx * P["fix_dx"], y) for sx in (-1, 1) for y in (g_, L["W_in"] - g_)]
    return L


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
def rbox(x0, y0, z0, x1, y1, z1, r=0.0):
    """Axis-aligned box, optionally with rounded vertical edges."""
    wp = cq.Workplane("XY", origin=(0, 0, z0)).center((x0 + x1) / 2, (y0 + y1) / 2)
    s = wp.rect(x1 - x0, y1 - y0).extrude(z1 - z0)
    if r > 0:
        s = s.edges("|Z").fillet(r)
    return s


def cyl(p, d, length, axis):
    """Cylinder of diameter d starting at point p, running `length` along axis."""
    return cq.Workplane("XY").add(cq.Solid.makeCylinder(d / 2, length, cq.Vector(*p), cq.Vector(*axis)))


def cyl_z(x, y, z0, z1, d):
    return cyl((x, y, z0), d, z1 - z0, (0, 0, 1))


def cyl_x(x0, x1, y, z, d, flat=0.0):
    c = cyl((x0, y, z), d, x1 - x0, (1, 0, 0))
    if flat > 0:   # D-hole: remove everything above the flat
        c = c.cut(rbox(x0 - 1, y - d, z - d / 2 + flat, x1 + 1, y + d, z + d))
    return c


def slot_x(x0, x1, y, z, w, h, r):
    """Rounded rectangular cutter running along X (through an end wall)."""
    s = rbox(x0, y - w / 2, z - h / 2, x1, y + w / 2, z + h / 2)
    if r > 0:
        s = s.edges("|X").fillet(min(r, w / 2 - 0.01, h / 2 - 0.01))
    return s


def outline_2d(P, L, z0, z1):
    """Outer body: a plain rounded rectangle. The screw pillars are inside it."""
    w = P["wall"]
    return rbox(-w, -w, z0, L["L_in"] + w, L["W_in"] + w, z1, P["outer_r"])


_CAVITY_WIRE = {}


def cavity_wire(P, L):
    """Plan outline of the cavity: rounded rectangle with the screw pillars bitten out."""
    key = (L["L_in"], L["W_in"], tuple(L["lugs"]))
    if key not in _CAVITY_WIRE:
        Li, Wi = L["L_in"], L["W_in"]
        s = rbox(0, 0, 0, Li, Wi, 1)
        for (lx, ly) in L["lugs"]:
            s = s.cut(cyl_z(lx, ly, -1, 2, 2 * P["lug_r"]))

        # blend only the edges where a pillar meets a wall (not the cylinder seams)
        def on_wall(e):
            c = e.Center()
            return min(abs(c.x), abs(c.x - Li), abs(c.y), abs(c.y - Wi)) < 1e-6
        s = s.newObject([e for e in s.edges("|Z").vals() if on_wall(e)]).fillet(P["pillar_blend"])
        _CAVITY_WIRE[key] = s.faces("<Z").val().outerWire()
    return _CAVITY_WIRE[key]


def cavity_offset(P, L, d):
    """Workplane holding the cavity outline offset by d (+ = into the wall)."""
    wp = cq.Workplane("XY").add(cavity_wire(P, L)).toPending()
    return wp.offset2D(d, "arc") if abs(d) > 1e-9 else wp


def cavity_prism(P, L, d, z0, z1):
    return cavity_offset(P, L, d).extrude(z1 - z0).translate((0, 0, z0))


def cavity_ring(P, L, d_out, d_in, z0, z1):
    return cavity_prism(P, L, d_out, z0, z1).cut(cavity_prism(P, L, d_in, z0 - 1, z1 + 1))


# --------------------------------------------------------------------------
# Base
# --------------------------------------------------------------------------
def make_base(P, L):
    w, f = P["wall"], P["floor"]
    L_in, W_in, H_in = L["L_in"], L["W_in"], L["H_in"]

    base = outline_2d(P, L, -f, H_in)
    if P["edge_r"] > 0:
        base = base.faces("<Z").edges().fillet(P["edge_r"])
    base = base.cut(cavity_prism(P, L, 0.0, 0, H_in + 1))

    # gasket groove in the top of the wall
    go, gw, gd = P["groove_offset"], P["groove_w"], P["groove_depth"]
    base = base.cut(cavity_ring(P, L, go + gw / 2, go - gw / 2, H_in - gd, H_in + 1))

    # lid screw pilot holes in the pillars
    for (lx, ly) in L["lugs"]:
        base = base.cut(cyl_z(lx, ly, H_in - P["lug_hole_depth"], H_in + 1, P["lug_hole"]))

    # blind holes for the pipe clamp, up into bosses beside the long walls
    if P["fix_holes"]:
        r = P["fix_boss_d"] / 2
        for (fx, fy) in L["fix"]:
            yw = -0.1 if fy < W_in / 2 else W_in + 0.1
            base = base.union(cyl_z(fx, fy, 0, P["fix_boss_h"], P["fix_boss_d"]))
            base = base.union(rbox(fx - r, min(fy, yw), 0, fx + r, max(fy, yw), P["fix_boss_h"]))
            base = base.cut(cyl_z(fx, fy, -f - 1, P["fix_boss_h"] - P["fix_hole_cap"], P["fix_hole"]))

    # PCB standoffs
    for (hx, hy) in L["holes"]:
        base = base.union(cyl_z(hx, hy, 0, P["standoff_h"], P["standoff_d"]))
        base = base.union(
            cq.Workplane("XY", origin=(hx, hy, 0)).circle(P["standoff_flare_d"] / 2)
            .workplane(offset=P["standoff_flare_h"]).circle(P["standoff_d"] / 2).loft())
        base = base.cut(cyl_z(hx, hy, P["standoff_h"] - P["standoff_hole_depth"],
                              P["standoff_h"] + 1, P["standoff_hole"]))

    # battery bay retaining ribs (three sides; +X side is open to the SMA bay)
    bx0, by0, bx1, by1 = L["batt"]
    t, h = P["batt_rib_t"], P["batt_rib_h"]
    base = base.union(rbox(bx0 - t, by0 - 0.5, 0, bx1 + 0.01, by1 + t, h))
    base = base.cut(rbox(bx0, by0 - 1, -1, bx1 + 1, by1, h + 1))
    # short end stop at +X so the holder can't slide into the SMA bay, with a wire notch
    base = base.union(rbox(bx1, by0 - 0.5, 0, bx1 + t, by1 + t, h))
    base = base.cut(rbox(bx1 - 0.01, by0 + 6, -1, bx1 + t + 1, by1 - 6, h + 1))
    if P["batt_screw_pitch"] > 0:
        cx, cy = 0.5 * (bx0 + bx1), 0.5 * (by0 + by1)
        for sx in (cx - P["batt_screw_pitch"] / 2, cx + P["batt_screw_pitch"] / 2):
            base = base.cut(cyl_z(sx, cy, -f - 1, 1, 3.4))

    # charger / boost module bay: low ribs all round, open at the corners for wires
    mx0, my0, mx1, my1 = L["mod"]
    t, h = P["mod_rib_t"], P["mod_rib_h"]
    base = base.union(rbox(mx0 - t, my0 - t, 0, mx1 + t, my1 + t, h))
    base = base.cut(rbox(mx0, my0, -1, mx1, my1, h + 1))
    for (cx, cy) in ((mx0, my0), (mx1, my0), (mx0, my1), (mx1, my1)):
        base = base.cut(rbox(cx - 4, cy - 4, -1, cx + 4, cy + 4, h + 1))

    # ---- wall openings ----
    # +X wall: SMA bulkhead
    base = base.cut(cyl_x(L_in - 1, L_in + w + 1, L["sma_y"], L["sma_z"], P["sma_d"], P["sma_flat"]))
    # +X wall: optional micro-USB opening for the breakout's own connector
    if P["usb_mode"] in ("direct", "panel"):
        base = base.cut(slot_x(L_in - 1, L_in + w + 1, L["usb_y"], L["usb_z"],
                               P["usb_cut_w"], P["usb_cut_h"], P["usb_cut_r"]))
    if P["usb_mode"] == "panel":
        for sy in (L["usb_y"] - P["usb_panel_screw_pitch"] / 2, L["usb_y"] + P["usb_panel_screw_pitch"] / 2):
            base = base.cut(cyl_x(L_in - 1, L_in + w + 1, sy, L["usb_z"], P["usb_panel_screw_d"]))
    # -X wall: charge jack and switch
    base = base.cut(cyl_x(-w - 1, 1, L["charge_y"], P["charge_z"], P["charge_d"]))
    base = base.cut(cyl_x(-w - 1, 1, L["sw_y"], P["sw_z"], P["sw_d"]))
    # -X wall: optional vent above the battery bay
    if P["vent_d"] > 0:
        base = base.cut(cyl_x(-w - 1, 1, L["vent_y"], P["vent_z"], P["vent_d"]))
    return base


# --------------------------------------------------------------------------
# Lid
# --------------------------------------------------------------------------
def make_lid(P, L):
    w = P["wall"]
    L_in, W_in, H_in = L["L_in"], L["W_in"], L["H_in"]
    z0 = H_in
    lid = outline_2d(P, L, z0, z0 + P["lid_t"])
    if P["edge_r"] > 0:
        lid = lid.faces(">Z").edges().fillet(P["edge_r"])
    # locating lip inside the cavity, following the pillars
    c = P["lip_clear"]
    lid = lid.union(cavity_ring(P, L, -c, -c - P["lip_t"], z0 - P["lip_h"], z0))
    # screw holes with counterbores
    for (lx, ly) in L["lugs"]:
        lid = lid.cut(cyl_z(lx, ly, z0 - 1, z0 + P["lid_t"] + 1, P["lid_screw_d"]))
        lid = lid.cut(cyl_z(lx, ly, z0 + P["lid_t"] - P["lid_cbore_depth"], z0 + P["lid_t"] + 1, P["lid_cbore_d"]))
    # LED windows
    for (lx, ly) in L["leds"]:
        if P["led_window"] == "hole":
            lid = lid.cut(cyl_z(lx, ly, z0 - 1, z0 + P["lid_t"] + 1, P["led_hole_d"]))
        elif P["led_window"] == "thin":
            lid = lid.cut(cyl_z(lx, ly, z0 - 1, z0 + P["lid_t"] - P["led_skin"], P["led_window_d"]))
    return lid


# --------------------------------------------------------------------------
# Pipe clamp for the back face (two printed parts: saddle + cap)
# --------------------------------------------------------------------------
def clamp_dims(P, L, od):
    f = P["floor"]
    D = dict(xc=L["L_in"] / 2, yc=L["W_in"] / 2, top=-f)
    D["zc"] = -f - P["clamp_min_t"] - od / 2          # pipe axis (runs along X)
    D["z_saddle"] = D["zc"] + P["clamp_gap"] / 2      # saddle parting face
    D["z_cap"] = D["zc"] - P["clamp_gap"] / 2         # cap parting face
    D["half_w"] = od / 2 + P["clamp_flange"]
    D["bolts"] = [(D["xc"] + sx * P["clamp_bolt_dx"], D["yc"] + sy * (od / 2 + P["clamp_bolt_off"]))
                  for sx in (-1, 1) for sy in (-1, 1)]
    D["nut_seat"] = D["z_saddle"] + P["clamp_nut_seat"]
    return D


def make_pipe_clamp(P, L, od):
    """Saddle screws to the four blind holes in the back; the cap clamps the pipe with 4 x M4."""
    D = clamp_dims(P, L, od)
    xc, yc, zc, top = D["xc"], D["yc"], D["zc"], D["top"]
    hl, hw = P["clamp_len"] / 2, D["half_w"]
    bore = cyl_x(xc - hl - 1, xc + hl + 1, yc, zc, od + 0.4)
    ear = max(abs(fy - yc) for _, fy in L["fix"]) + 5.0
    zb = top - P["clamp_plate_t"]

    saddle = rbox(xc - hl, yc - ear, zb, xc + hl, yc + ear, top, 3.0)
    saddle = saddle.union(rbox(xc - hl, yc - hw, D["z_saddle"], xc + hl, yc + hw, top)).cut(bore)
    for (bx, by) in D["bolts"]:
        saddle = saddle.cut(cyl_z(bx, by, D["z_saddle"] - 1, top + 1, P["clamp_bolt_d"]))
        saddle = saddle.cut(cq.Workplane("XY", origin=(bx, by, D["nut_seat"]))
                            .polygon(6, P["clamp_nut_af"] / 0.8660254).extrude(top + 1 - D["nut_seat"]))
    for (fx, fy) in L["fix"]:
        saddle = saddle.cut(cyl_z(fx, fy, zb - 1, top + 1, P["clamp_screw_d"]))
        saddle = saddle.cut(cyl_z(fx, fy, zb - 1, zb + P["clamp_cbore_depth"], P["clamp_cbore_d"]))

    cap = rbox(xc - hl, yc - hw, D["z_cap"] - P["clamp_plate_t"], xc + hl, yc + hw, D["z_cap"], 2.0)
    cap = cap.union(cyl_x(xc - hl, xc + hl, yc, zc, od + 0.4 + 2 * P["clamp_ring_t"]))
    cap = cap.cut(rbox(xc - hl - 1, yc - hw - 1, D["z_cap"], xc + hl + 1, yc + hw + 1, top + 1)).cut(bore)
    for (bx, by) in D["bolts"]:
        cap = cap.cut(cyl_z(bx, by, D["z_cap"] - P["clamp_plate_t"] - 1, D["z_cap"] + 1, P["clamp_bolt_d"]))
    return saddle, cap


# --------------------------------------------------------------------------
# Antenna mount for the top of the pipe
# --------------------------------------------------------------------------
def _ear_wedge(P, r_in, r_out, z0):
    """Ramp from the pinch ears up to the sleeve so they print without support (parts print upside-down)."""
    ew, el, eh, s = P["ant_ear_w"], P["ant_ear_len"], P["ant_ear_h"], P["ant_slot"]
    zt = z0 + eh
    return (cq.Workplane("XZ").polyline([(r_in + 1, zt), (r_out + el, zt), (r_in + 1, zt + el - 2.0)]).close()
            .extrude(s / 2 + ew, both=True))


def make_antenna_mount(P, od):
    """Pinch-clamp socket for the pipe end with a flat tray for a metal ground-plane disc.

    Local frame: Z is the pipe axis, the pipe end butts against the tray underside at Z = 0,
    the sleeve hangs below, the tray top (where the disc goes) is at Z = ant_tray_t.
    The pinch slot and ears point along +X.
    """
    import math
    r_in = od / 2 + 0.2
    r_out = r_in + P["ant_sleeve_wall"]
    sl, tt, R = P["ant_sleeve_len"], P["ant_tray_t"], P["ant_gp_d"] / 2
    m = cyl_z(0, 0, 0, tt, 2 * R).union(cyl_z(0, 0, -sl, 0, 2 * r_out))
    # ribs from the sleeve out under the tray, none on the slot side
    n = P["ant_ribs"]
    for i in range(n):
        a = 360.0 / (n + 1) * (i + 1)
        rib = (cq.Workplane("XZ").polyline([(r_out - 1, 0), (R - 8, 0), (r_out - 1, -sl + 6)]).close()
               .extrude(P["ant_rib_t"] / 2, both=True).rotate((0, 0, 0), (0, 0, 1), a))
        m = m.union(rib)
    # pinch ears near the open end of the sleeve
    ew, el, eh, s = P["ant_ear_w"], P["ant_ear_len"], P["ant_ear_h"], P["ant_slot"]
    z0 = -sl + 3
    m = m.union(rbox(r_in + 1, -(s / 2 + ew), z0, r_out + el, s / 2 + ew, z0 + eh))
    m = m.union(_ear_wedge(P, r_in, r_out, z0))
    m = m.cut(cyl_z(0, 0, -sl - 1, 0, 2 * r_in))
    m = m.cut(rbox(0, -s / 2, -sl - 1, r_out + el + 1, s / 2, -4))          # slot stops 4 mm under the tray
    bx, bz = r_out + el / 2 + 1, z0 + eh / 2
    m = m.cut(cyl((bx, -(s / 2 + ew) - 1, bz), 4.4, s + 2 * ew + 2, (0, 1, 0)))
    m = m.cut(cq.Workplane("XZ", origin=(bx, -(s / 2 + ew) + 3.0, bz)).polygon(6, 7.2 / 0.8660254).extrude(4))  # M4 nut
    for i in range(3):
        a = math.radians(60 + 120 * i)
        m = m.cut(cyl_z(P["ant_disc_screw_r"] * math.cos(a), P["ant_disc_screw_r"] * math.sin(a), -1, tt + 1,
                        P["ant_disc_screw_d"]))
    return m


def make_helical_cap(P, od):
    """Pipe-top cap for a helical (drone-type) antenna: no ground plane needed.

    Same pinch-clamp sleeve as the ANN-MB mount. Above the pipe end there is a small
    chamber for the back of an SMA bulkhead fitted through the top; the cable leaves
    through a side window (opposite the pinch slot) and runs down the outside of the pole.
    Local frame as make_antenna_mount: pipe end at Z = 0, pipe axis = Z.
    """
    r_in = od / 2 + 0.2
    r_out = r_in + P["ant_sleeve_wall"]
    sl, ch, tt = P["ant_sleeve_len"], P["hel_chamber_h"], P["hel_top_t"]
    m = cyl_z(0, 0, -sl, ch + tt, 2 * r_out)
    ew, el, eh, s = P["ant_ear_w"], P["ant_ear_len"], P["ant_ear_h"], P["ant_slot"]
    z0 = -sl + 3
    m = m.union(rbox(r_in + 1, -(s / 2 + ew), z0, r_out + el, s / 2 + ew, z0 + eh))
    m = m.union(_ear_wedge(P, r_in, r_out, z0))
    m = m.faces(">Z").edges().fillet(2.0)
    m = m.cut(cyl_z(0, 0, -sl - 1, 0, 2 * r_in))
    m = m.cut(cyl_z(0, 0, -1, ch, 2 * (r_in - 2.5)))                          # chamber; the step is the pipe stop
    m = m.cut(cyl_z(0, 0, ch - 1, ch + tt + 1, P["hel_sma_d"]))
    m = m.cut(rbox(-r_out - 1, -P["hel_window_w"] / 2, 1.0, 0, P["hel_window_w"] / 2, 1.0 + P["hel_window_h"], 0)
              .edges("|X").fillet(3.0))
    m = m.cut(rbox(0, -s / 2, -sl - 1, r_out + el + 1, s / 2, -4))
    bx, bz = r_out + el / 2 + 1, z0 + eh / 2
    m = m.cut(cyl((bx, -(s / 2 + ew) - 1, bz), 4.4, s + 2 * ew + 2, (0, 1, 0)))
    m = m.cut(cq.Workplane("XZ", origin=(bx, -(s / 2 + ew) + 3.0, bz)).polygon(6, 7.2 / 0.8660254).extrude(4))
    return m


def make_survey_cap(P, od):
    """Pipe-top 5/8"-11 adapter for a survey antenna (own ground plane, screws onto the bolt).

    The bolt goes in from below before the cap goes on the pipe; its head sits in a hex
    pocket and the pipe end stops it dropping out. Local frame as make_antenna_mount.
    """
    r_in = od / 2 + 0.2
    r_out = max(r_in + P["ant_sleeve_wall"], P["srv_body_r_min"])
    sl, hp, tt = P["ant_sleeve_len"], P["srv_hex_h"], P["srv_top_t"]
    m = cyl_z(0, 0, -sl, hp + tt, 2 * r_out)
    ew, el, eh, s = P["ant_ear_w"], P["ant_ear_len"], P["ant_ear_h"], P["ant_slot"]
    z0 = -sl + 3
    m = m.union(rbox(r_in + 1, -(s / 2 + ew), z0, r_out + el, s / 2 + ew, z0 + eh))
    m = m.union(_ear_wedge(P, r_in, r_out, z0))
    m = m.faces(">Z").edges().fillet(2.0)
    m = m.cut(cyl_z(0, 0, -sl - 1, 0, 2 * r_in))
    m = m.cut(cq.Workplane("XY", origin=(0, 0, -0.01)).polygon(6, P["srv_hex_af"] / 0.8660254).extrude(hp + 0.01))
    m = m.cut(cyl_z(0, 0, hp - 1, hp + tt + 1, P["srv_bolt_d"]))
    m = m.cut(rbox(0, -s / 2, -sl - 1, r_out + el + 1, s / 2, -4))
    bx, bz = r_out + el / 2 + 1, z0 + eh / 2
    m = m.cut(cyl((bx, -(s / 2 + ew) - 1, bz), 4.4, s + 2 * ew + 2, (0, 1, 0)))
    m = m.cut(cq.Workplane("XZ", origin=(bx, -(s / 2 + ew) + 3.0, bz)).polygon(6, 7.2 / 0.8660254).extrude(4))
    return m


# --------------------------------------------------------------------------
# Component keep-outs (fit report and preview)
# --------------------------------------------------------------------------
def make_keepouts(P, L):
    g = GERBER
    K = {}
    x0, y0, x1, y1 = L["pcb_box"]
    zb = P["standoff_h"]
    pcb = rbox(x0, y0, zb, x1, y1, zb + P["pcb_t"], P["pcb_corner_r"])
    for (hx, hy) in L["holes"]:
        pcb = pcb.cut(cyl_z(hx, hy, zb - 1, zb + 3, P["hole_d"]))
    K["breakout PCB"] = pcb
    rc = 0.5 * (g["micro_rows"][0] + g["micro_rows"][1])
    K["simpleRTK2B Micro"] = gbox(L, rc - P["micro_w"] / 2, rc + P["micro_w"] / 2, g["micro_y"][0], g["micro_y"][1],
                                  zb + P["pcb_t"], zb + P["pcb_t"] + P["micro_h"])
    if P["micro_ant"] == "sma":
        za = zb + P["pcb_t"] + 6.3            # Micro board centre plane = SMA jack axis (est.)
        K["Micro SMA + plug"] = gbox(L, rc - P["micro_sma_d"] / 2, rc + P["micro_sma_d"] / 2,
                                     g["micro_y"][1], g["micro_y"][1] + P["micro_sma_len"],
                                     za - P["micro_sma_d"] / 2, za + P["micro_sma_d"] / 2)
    hc = 0.5 * (g["x_min"] + g["x_max"])
    K["HC-05"] = gbox(L, hc - P["hc05_w"] / 2, hc + P["hc05_w"] / 2, g["hc05_y"][0], g["hc05_y"][1],
                      zb - P["hc05_h"], zb)
    if P["usb_mode"] != "none":
        K["USB plug"] = rbox(L["usb_face_x"], L["usb_y"] - 5.5, L["usb_z"] - 3.75,
                             L["L_in"] + P["wall"] + 20, L["usb_y"] + 5.5, L["usb_z"] + 3.75, 1.5)
    bx0, by0, bx1, by1 = L["batt"]
    K["18650 holder"] = rbox(bx0, by0, 0, bx1, by1, P["batt_h"])
    mx0, my0, mx1, my1 = L["mod"]
    K["charger/boost module"] = rbox(mx0, my0, 0, mx1, my1, P["mod_h"])
    K["SMA jack"] = cyl_x(L["L_in"] - P["sma_body_len"], L["L_in"], L["sma_y"], L["sma_z"], 9.0)
    K["charge jack"] = cyl_x(0, P["charge_body_depth"], L["charge_y"], P["charge_z"], P["charge_body_d"])
    K["switch body"] = rbox(0, L["sw_y"] - P["sw_body_w"] / 2, P["sw_z"] - P["sw_body_h"] / 2,
                            P["sw_body_depth"], L["sw_y"] + P["sw_body_w"] / 2, P["sw_z"] + P["sw_body_h"] / 2)
    return K


def vol(shape):
    return shape.val().Volume() if shape.vals() else 0.0


def fit_report(P, L, base, lid, K):
    w = P["wall"]
    print("\n=== Layout ===")
    print(f"cavity        : {L['L_in']:.1f} x {L['W_in']:.1f} x {L['H_in']:.1f} mm  (L x W x H)")
    print(f"box body      : {L['L_in'] + 2 * w:.1f} x {L['W_in'] + 2 * w:.1f} x "
          f"{L['H_in'] + P['floor'] + P['lid_t']:.1f} mm  (with lid)")
    print(f"outside       : smooth shell, screw pillars inside (corner r {P['outer_r']}, edge fillet {P['edge_r']})")
    x0, y0, x1, y1 = L["pcb_box"]
    print(f"PCB           : X {x0:.2f}..{x1:.2f}  Y {y0:.2f}..{y1:.2f}  Z {P['standoff_h']:.1f}..{P['standoff_h'] + P['pcb_t']:.1f}")
    print("standoffs     : " + ", ".join(f"({x:.2f},{y:.2f})" for x, y in L["holes"]))
    print(f"Micro antenna : {P['micro_ant']}" + ("  (breakout turned 180 deg, ANT end towards -X)" if L["bs"] < 0 else ""))
    print(f"SMA           : +X wall, Y={L['sma_y']:.2f} Z={L['sma_z']:.2f}, d={P['sma_d']}")
    print(f"micro-USB     : mode={P['usb_mode']}, +X wall, Y={L['usb_y']:.2f} Z={L['usb_z']:.2f}")
    print(f"charge jack   : -X wall, Y={L['charge_y']:.2f} Z={P['charge_z']:.2f}, d={P['charge_d']}")
    print(f"switch        : -X wall, Y={L['sw_y']:.2f} Z={P['sw_z']:.2f}, d={P['sw_d']}")
    print(f"module bay    : X {L['mod'][0]:.1f}..{L['mod'][2]:.1f}  Y {L['mod'][1]:.1f}..{L['mod'][3]:.1f}")
    print(f"battery bay   : X {L['batt'][0]:.1f}..{L['batt'][2]:.1f}  Y {L['batt'][1]:.1f}..{L['batt'][3]:.1f}")
    print("LED windows   : " + ", ".join(f"({x:.2f},{y:.2f})" for x, y in L["leds"]))
    print("lid screws    : " + ", ".join(f"({x:.1f},{y:.1f})" for x, y in L["lugs"]))
    cord = cavity_offset(P, L, P["groove_offset"]).vals()[0].Length()
    print(f"gasket cord   : {P['cord_d']} mm cord, groove {P['groove_w']} x {P['groove_depth']} mm, "
          f"centre-line length {cord:.0f} mm (cut ~{cord + 5:.0f} mm)")

    if P["fix_holes"]:
        print("clamp holes   : " + ", ".join(f"({x:.1f},{y:.1f})" for x, y in L["fix"]) +
              f"  blind M3, {P['floor'] + P['fix_boss_h'] - P['fix_hole_cap']:.1f} mm deep from the back face")
    print("\n=== Fit check (keep-out volume intersecting the printed parts, mm^3) ===")
    ok = True
    names = list(K)
    for n in names:
        vb, vl = vol(base.intersect(K[n])), vol(lid.intersect(K[n]))
        flag = "ok " if (vb + vl) < 0.01 else "CLASH"
        ok &= flag == "ok "
        print(f"  {flag}  {n:22s} base {vb:8.2f}   lid {vl:8.2f}")
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            v = vol(K[a].intersect(K[b]))
            if v > 0.01:
                ok = False
                print(f"  CLASH  {a} <-> {b}: {v:.2f}")
    print("  component-to-component: " + ("no clashes" if ok else "see above"))
    return ok


# --------------------------------------------------------------------------
# Preview drawings: 2D cross-sections (matplotlib, headless)
# --------------------------------------------------------------------------
def _slice(shape, axis, at, thick=0.2):
    big = 500
    if axis == "z":
        slab = rbox(-big, -big, at - thick / 2, big, big, at + thick / 2)
    elif axis == "y":
        slab = rbox(-big, at - thick / 2, -big, big, at + thick / 2, big)
    else:
        slab = rbox(at - thick / 2, -big, -big, at + thick / 2, big, big)
    return shape.intersect(slab)


def _draw(ax, shape, color, alpha, axis):
    from matplotlib.patches import Polygon
    if not shape.vals():
        return
    for solid in shape.solids().vals():
        verts, tris = solid.tessellate(0.1, 0.3)
        pts = [(p.x, p.y) if axis == "z" else ((p.x, p.z) if axis == "y" else (p.y, p.z)) for p in verts]
        for t in tris:
            ax.add_patch(Polygon([pts[i] for i in t], closed=True, facecolor=color,
                                 edgecolor=color, linewidth=0.3, alpha=alpha))


def section_png(path, title, axis, at, parts, xlabel, ylabel):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(12, 8), dpi=110)
    for shape, color, alpha in parts:
        _draw(ax, _slice(shape, axis, at), color, alpha, axis)
    ax.set_aspect("equal")
    ax.autoscale_view()
    ax.relim(); ax.autoscale()
    ax.grid(True, linewidth=0.3)
    ax.set_xlabel(xlabel); ax.set_ylabel(ylabel)
    ax.set_title(title)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def render_all(P, L, base, lid, K, out):
    colors = {
        "breakout PCB": "#2a6fdb", "simpleRTK2B Micro": "#1b3f8f", "HC-05": "#2e8b57",
        "USB plug": "#555555", "18650 holder": "#c0392b", "charger/boost module": "#8e44ad",
        "SMA jack": "#d4a017", "Micro SMA + plug": "#b8860b", "charge jack": "#7f8c8d", "switch body": "#e67e22",
    }
    comp = [(K[n], colors[n], 0.55) for n in K]
    grey = ("#404040", 1.0)
    H = L["H_in"]
    plans = [
        ("plan_z0.png", "Plan section 0.3 mm above the floor: ribs, tabs, standoff flares", 0.3),
        ("plan_z11.png", "Plan section at Z = 11 mm: wall openings, standoffs, component keep-outs", 11.0),
        ("plan_groove.png", f"Plan section at Z = {H - 0.7:.1f} mm: gasket groove and screw pillars", H - 0.7),
    ]
    for fn, title, z in plans:
        section_png(os.path.join(out, fn), title, "z", z, [(base, *grey)] + comp, "X (mm)", "Y (mm)")
    section_png(os.path.join(out, "wall_plusX.png"), "+X end wall (viewed from outside is mirrored): SMA bulkhead" +
                (", micro-USB" if P["usb_mode"] != "none" else ""), "x", L["L_in"] + P["wall"] / 2,
                [(base, *grey)] + comp, "Y (mm)", "Z (mm)")
    section_png(os.path.join(out, "wall_minusX.png"), "-X end wall: charge jack and switch", "x", -P["wall"] / 2,
                [(base, *grey)] + comp, "Y (mm)", "Z (mm)")
    section_png(os.path.join(out, "lid_plate.png"), "Lid section through the plate: screw holes and LED window pockets",
                "z", H + 0.5, [(lid, *grey)], "X (mm)", "Y (mm)")
    section_png(os.path.join(out, "lid_lip.png"), "Lid section just under the plate: locating lip",
                "z", H - 0.5, [(lid, *grey)], "X (mm)", "Y (mm)")
    section_png(os.path.join(out, "side_section.png"), f"Long section at Y = {L['sma_y']:.1f} mm through the battery bay and SMA",
                "y", L["sma_y"], [(base, *grey), (lid, "#808080", 1.0)] + comp, "X (mm)", "Z (mm)")


def main():
    L = layout(P)
    base = make_base(P, L)
    lid = make_lid(P, L)
    K = make_keepouts(P, L)
    ok = fit_report(P, L, base, lid, K)

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), P["out_dir"])
    os.makedirs(out, exist_ok=True)
    cq.exporters.export(base, os.path.join(out, "rtk_enclosure_base.stl"), tolerance=P["stl_tol"])
    cq.exporters.export(base, os.path.join(out, "rtk_enclosure_base.step"))
    # lid STL is flipped so its flat top prints on the bed
    lid_print = lid.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, 0, L["H_in"] + P["lid_t"]))
    cq.exporters.export(lid_print, os.path.join(out, "rtk_enclosure_lid.stl"), tolerance=P["stl_tol"])
    cq.exporters.export(lid, os.path.join(out, "rtk_enclosure_lid.step"))
    for od in P["pipe_variants"]:
        tag = f"od{od:g}".replace(".", "p")
        for nm, shp in zip(("saddle", "cap"), make_pipe_clamp(P, L, od)):
            cq.exporters.export(shp, os.path.join(out, f"pipe_clamp_{nm}_{tag}.step"))
            # STL flipped: saddle plate / cap parting face down on the bed
            shp = shp.rotate((0, 0, 0), (1, 0, 0), 180)
            shp = shp.translate((0, 0, -shp.val().BoundingBox().zmin))
            cq.exporters.export(shp, os.path.join(out, f"pipe_clamp_{nm}_{tag}.stl"), tolerance=P["stl_tol"])
        mount = make_antenna_mount(P, od)
        cq.exporters.export(mount, os.path.join(out, f"antenna_mount_{tag}.step"))
        mount = mount.rotate((0, 0, 0), (1, 0, 0), 180)       # tray face down on the bed, sleeve up
        mount = mount.translate((0, 0, -mount.val().BoundingBox().zmin))
        cq.exporters.export(mount, os.path.join(out, f"antenna_mount_{tag}.stl"), tolerance=P["stl_tol"])
        hcap = make_helical_cap(P, od)
        cq.exporters.export(hcap, os.path.join(out, f"helical_cap_{tag}.step"))
        hcap = hcap.rotate((0, 0, 0), (1, 0, 0), 180)         # flat top on the bed
        hcap = hcap.translate((0, 0, -hcap.val().BoundingBox().zmin))
        cq.exporters.export(hcap, os.path.join(out, f"helical_cap_{tag}.stl"), tolerance=P["stl_tol"])
        scap = make_survey_cap(P, od)
        cq.exporters.export(scap, os.path.join(out, f"survey_cap_{tag}.step"))
        scap = scap.rotate((0, 0, 0), (1, 0, 0), 180)         # flat top on the bed
        scap = scap.translate((0, 0, -scap.val().BoundingBox().zmin))
        cq.exporters.export(scap, os.path.join(out, f"survey_cap_{tag}.stl"), tolerance=P["stl_tol"])
    for name, shp in (("base", base), ("lid (print orientation)", lid_print)):
        bb = shp.val().BoundingBox()
        print(f"{name:24s} STL extents: {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm, z from {bb.zmin:.1f}")

    render_all(P, L, base, lid, K, out)
    print(f"\nwrote STL/STEP/PNG to {out}")
    if not ok:
        print("WARNING: clashes reported above")
        sys.exit(1)


if __name__ == "__main__":
    main()
