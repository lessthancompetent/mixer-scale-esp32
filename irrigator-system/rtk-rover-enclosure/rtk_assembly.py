#!/usr/bin/env python3
"""
Mocked-up internals and a coloured STEP assembly for the RTK rover enclosure.

rtk_enclosure.py checks the printed parts against plain keep-out boxes. This
script puts recognisable dummy components in the same places (breakout with
its sockets and micro-USB, simpleRTK2B Micro with the ZED-F9P can and U.FL,
HC-05 on its carrier, 18650 cell in a holder, PowerBoost-style module, SMA
bulkhead with pigtail, booted toggle switch, capped GX12 socket, the power
wiring, lid gasket, screws, and the pipe clamp on a length of PVC pipe),
checks them against the printed parts and against each other, and writes

    out/rtk_rover_assembly.step     coloured, one named part per component

The mock-ups are visual/fit aids built from datasheet-ish dimensions, not
vendor CAD. Anything marked "est." below is a guess to be replaced with a
calliper measurement.

Run:   python3 rtk_assembly.py
Then:  render_freecad.py (inside FreeCAD) turns the STEP into shaded PNGs.
"""
import math
import os
import sys

import cadquery as cq

from rtk_enclosure import (GERBER, P, cavity_ring, clamp_dims, cyl, cyl_x, cyl_z, gxy, layout, make_antenna_mount,
                           make_helical_cap, make_phone_bracket, make_phone_cradle, make_phone_jaw, make_phone_link, make_phone_tongue,
                           make_survey_cap, phone_place,
                           make_base, make_lid, make_pipe_clamp, rbox, vol)
from rtk_enclosure import gbox as _gbox

COLORS = {
    "base": (0.85, 0.47, 0.10), "lid": (0.90, 0.55, 0.15),
    "gasket": (0.75, 0.10, 0.10),
    "pcb_green": (0.05, 0.35, 0.15), "pcb_blue": (0.08, 0.20, 0.55), "pcb_black": (0.08, 0.08, 0.10),
    "plastic_black": (0.10, 0.10, 0.10), "plastic_white": (0.92, 0.92, 0.88),
    "metal": (0.75, 0.76, 0.78), "gold": (0.83, 0.66, 0.22), "steel_dark": (0.30, 0.31, 0.33),
    "cell": (0.10, 0.50, 0.55), "rubber": (0.15, 0.15, 0.17), "coax": (0.25, 0.25, 0.25),
    "led_green": (0.10, 0.90, 0.20), "led_red": (0.95, 0.10, 0.10),
    "wire_red": (0.85, 0.05, 0.05), "wire_black": (0.04, 0.04, 0.04),
    "clamp": (0.20, 0.22, 0.25), "pvc": (0.88, 0.88, 0.86),
}

WIRE_D = 1.5     # 24 AWG silicone hook-up wire


def hex_x(x0, x1, y, z, af):
    """Hex prism along X, across-flats af, flats facing +-Z."""
    return cq.Workplane("YZ", origin=(x0, y, z)).polygon(6, af / math.cos(math.pi / 6)).extrude(x1 - x0)


def cone_x(x0, x1, y, z, d0, d1):
    return cq.Workplane("XY").add(cq.Solid.makeCone(d0 / 2, d1 / 2, abs(x1 - x0), cq.Vector(x0, y, z),
                                                    cq.Vector(1 if x1 > x0 else -1, 0, 0)))


def wire(pts, d=WIRE_D):
    """Bent wire: straight runs through pts with rounded elbows."""
    out = None
    for i, (a, b) in enumerate(zip(pts, pts[1:])):
        va, vb = cq.Vector(*a), cq.Vector(*b)
        seg = cyl(a, d, (vb - va).Length, (vb - va).toTuple())
        if i:
            seg = seg.union(cq.Workplane("XY").add(cq.Solid.makeSphere(d / 2, va)))
        out = seg if out is None else out.union(seg)
    return out


def make_mockups(P, L):
    """Return {part name: (shape, colour key)}. Same frame as the enclosure."""
    g = GERBER
    M = {}
    zb = P["standoff_h"]
    zt = zb + P["pcb_t"]

    def gbox(gx0, gx1, gy0, gy1, z0, z1, r=0.0):
        """Box given in gerber x/y (board frame), enclosure Z."""
        return _gbox(L, gx0, gx1, gy0, gy1, z0, z1, r)

    # ---- breakout board ------------------------------------------------------
    x0, y0, x1, y1 = L["pcb_box"]
    pcb = rbox(x0, y0, zb, x1, y1, zt, P["pcb_corner_r"])
    for (hx, hy) in L["holes"]:
        pcb = pcb.cut(cyl_z(hx, hy, zb - 1, zt + 1, P["hole_d"]))
    M["breakout_pcb"] = (pcb, "pcb_green")

    sock_h, sock_y = 4.3, (4.5, 24.5)          # 1x10 2 mm sockets; position along the row est.
    socks = None
    for rx in g["micro_rows"]:
        s = gbox(rx - 1.0, rx + 1.0, sock_y[0], sock_y[1], zt, zt + sock_h)
        socks = s if socks is None else socks.union(s)
    M["breakout_sockets"] = (socks, "plastic_black")

    ux, uy = g["usb"]
    M["breakout_usb"] = (gbox(ux - 3.75, ux + 3.75, g["usb_face_y"] - 5.6, g["usb_face_y"], zt, zt + 2.6), "metal")
    for (lx, ly), c, n in zip(L["leds"], ("led_green", "led_red"), ("pwr", "rtk")):
        M[f"breakout_led_{n}"] = (rbox(lx - 0.8, ly - 0.4, zt, lx + 0.8, ly + 0.4, zt + 0.6), c)

    # ---- simpleRTK2B Micro on the sockets --------------------------------------
    rc = 0.5 * (g["micro_rows"][0] + g["micro_rows"][1])
    zm = zt + sock_h + 1.5                       # 1.5 = pin header plastic
    pins = None
    for rx in g["micro_rows"]:
        s = gbox(rx - 1.0, rx + 1.0, sock_y[0], sock_y[1], zt + sock_h, zm)
        pins = s if pins is None else pins.union(s)
    M["micro_headers"] = (pins, "plastic_black")
    M["micro_pcb"] = (gbox(rc - 11.75, rc + 11.75, -0.5, 29.5, zm, zm + 1.0, 1.0), "pcb_black")
    M["micro_zed_f9p"] = (gbox(rc - 8.5, rc + 8.5, 3.5, 25.5, zm + 1.0, zm + 3.4), "metal")
    sma_micro = P["micro_ant"] == "sma"
    ufl_gy = 27.5
    if not sma_micro:
        M["micro_ufl"] = (gbox(rc - 1.3, rc + 1.3, ufl_gy - 1.3, ufl_gy + 1.3, zm + 1.0, zm + 2.25), "gold")

    # ---- HC-05 on a ZS-040 carrier under the breakout ---------------------------
    hc = 0.5 * (g["x_min"] + g["x_max"])
    hy0 = g["hc05_y"][0] + 0.25
    zc = zb - 2.5                                # 2.5 = header plastic
    M["hc05_header"] = (gbox(hc - 7.6, hc + 7.6, hy0 + 0.5, hy0 + 3.0, zc, zb), "plastic_black")
    M["hc05_carrier"] = (gbox(hc - 7.75, hc + 7.75, hy0, hy0 + 37.0, zc - 1.2, zc), "pcb_blue")
    M["hc05_module"] = (gbox(hc - 6.5, hc + 6.5, hy0 + 9.0, hy0 + 36.0, zc - 1.2 - 0.8, zc - 1.2), "pcb_blue")
    M["hc05_shield"] = (gbox(hc - 5.5, hc + 5.5, hy0 + 10.0, hy0 + 28.0, zc - 1.2 - 0.8 - 2.2, zc - 1.2 - 0.8), "metal")

    # ---- 18650 holder and cell ----------------------------------------------------
    bx0, by0, bx1, by1 = L["batt"]
    hx0, hx1, hy_0, hy_1 = bx0 + 0.5, bx1 - 0.5, by0 + 0.5, by1 - 0.5
    cy = 0.5 * (hy_0 + hy_1)
    cell_d, cell_len, tray_f = 18.4, 65.0, 1.5
    cz = tray_f + cell_d / 2 + 0.2
    holder = rbox(hx0, hy_0, 0, hx1, hy_1, P["batt_h"], 1.0)
    holder = holder.cut(cyl_x(hx0 + 2.0, hx1 - 2.0, cy, cz, cell_d + 0.6))
    holder = holder.cut(rbox(hx0 + 2.0, cy - (cell_d + 0.6) / 2, cz, hx1 - 2.0, cy + (cell_d + 0.6) / 2, P["batt_h"] + 1))
    M["batt_holder"] = (holder, "plastic_black")
    cx0 = hx0 + 2.0 + 5.0                        # spring end at -X
    cell = cyl_x(cx0, cx0 + cell_len - 1.0, cy, cz, cell_d).union(cyl_x(cx0 + cell_len - 1.0, cx0 + cell_len, cy, cz, 6.0))
    M["batt_cell_18650"] = (cell, "cell")
    M["batt_spring"] = (cone_x(hx0 + 2.0, cx0, cy, cz, 5.0, 9.0), "metal")
    M["batt_contact"] = (rbox(cx0 + cell_len, cy - 4, cz - 4, hx1 - 2.0, cy + 4, cz + 4), "metal")

    # ---- charger / boost module (DD05CVSA: bare board with solder pads at both ends) -----------
    mx0, my0, mx1, my1 = L["mod"]
    mcx, mcy = 0.5 * (mx0 + mx1), 0.5 * (my0 + my1)
    zp = 1.0                                     # foam tape
    pl, pw = P["mod_pcb"]
    px0, py0, px1, py1 = mcx - pl / 2, mcy - pw / 2, mcx + pl / 2, mcy + pw / 2
    M["boost_tape"] = (rbox(px0 + 2, py0 + 2, 0, px1 - 2, py1 - 2, zp), "plastic_white")
    M["boost_pcb"] = (rbox(px0, py0, zp, px1, py1, zp + 1.2, 1.0), "pcb_blue")
    zq = zp + 1.2
    M["boost_inductor"] = (rbox(mcx + 1.5, mcy - 2.5, zq, mcx + 6.5, mcy + 2.5, zq + 3.0, 0.8), "steel_dark")
    M["boost_ics"] = (rbox(mcx - 5.5, mcy - 4.5, zq, mcx - 1.5, mcy - 1.0, zq + 1.0)
                      .union(rbox(mcx - 5.5, mcy + 0.5, zq, mcx - 2.5, mcy + 3.5, zq + 1.0)), "plastic_black")

    # ---- SMA bulkhead (+X wall) with U.FL pigtail ------------------------------------
    w, L_in = P["wall"], L["L_in"]
    sy, sz = L["sma_y"], L["sma_z"]
    sma_in = 8.0                                 # flange + crimp inside the wall (keep-out allows 15)
    sma = cyl_x(L_in - 2.0, L_in, sy, sz, 9.0)                          # inner flange
    sma = sma.union(cyl_x(L_in - sma_in, L_in - 2.0, sy, sz, 4.5))      # crimp ferrule
    sma = sma.union(cyl_x(L_in, L_in + w + 8.0, sy, sz, 6.35))          # 1/4-36 thread
    sma = sma.cut(cyl_x(L_in + w + 3.0, L_in + w + 9.0, sy, sz, 4.2))   # socket bore
    M["sma_bulkhead"] = (sma, "gold")
    M["sma_washer"] = (cyl_x(L_in + w, L_in + w + 0.8, sy, sz, 9.5).cut(cyl_x(L_in + w - 1, L_in + w + 2, sy, sz, 6.4)), "metal")
    M["sma_nut"] = (hex_x(L_in + w + 0.8, L_in + w + 3.0, sy, sz, 8.0).cut(cyl_x(L_in + w, L_in + w + 4, sy, sz, 6.4)), "gold")

    if sma_micro:
        # edge-mount SMA on the Micro, pointing -X over the charger module; right-angle plug, RG316 to the bulkhead
        ex, ey = gxy(L, rc, 29.5)                 # Micro board edge at the antenna end
        za = zm + 0.5
        jack = rbox(ex - 2.0, ey - 3.2, za - 3.2, ex + 3.5, ey + 3.2, za + 3.2).cut(
            rbox(ex - 0.01, ey - 4, zm, ex + 4, ey + 4, zm + 1.0))          # legs straddle the board
        M["micro_sma_jack"] = (jack.union(cyl_x(ex - 9.5, ex - 2.0, ey, za, 6.35)), "gold")
        px = ex - 14.0                            # right-angle plug body centre
        plug = cyl_x(ex - 9.5, ex - 2.2, ey, za, 9.0).cut(cyl_x(ex - 9.6, ex - 1.0, ey, za, 6.4))   # coupling nut
        plug = plug.union(rbox(px - 4.0, ey - 4.0, za - 4.0, ex - 9.5, ey + 4.0, za + 4.0))
        plug = plug.union(cyl((px, ey - 4.0, za), 5.0, 4.0, (0, -1, 0)))                         # crimp ferrule
        M["pigtail_sma_plug"] = (plug, "gold")
        cab_d, rb = 2.5, 8.0                      # RG316, bend radius
        x_in = L_in - sma_in
        pts = [(px, ey - 8.0, za), (px, sy + rb, za)]
        pts += [(px + rb - rb * math.cos(t), sy + rb - rb * math.sin(t), za + (sz - za) * t / (math.pi / 2))
                for t in [i * math.pi / 16 for i in range(1, 9)]]
        pts += [(x_in, sy, sz)]
        M["pigtail_coax"] = (wire(pts, cab_d), "coax")
    else:
        ufl_x, ufl_y = gxy(L, rc, ufl_gy)
        plug_z = zm + 2.25
        M["pigtail_ufl_plug"] = (rbox(ufl_x - 1.6, ufl_y - 2.6, plug_z, ufl_x + 1.6, ufl_y + 1.6, plug_z + 1.2, 0.5), "gold")
        cz_cable = plug_z + 0.6
        pts = [(ufl_x, ufl_y - 2.6, cz_cable), (ufl_x, ufl_y - 10.0, cz_cable),
               (ufl_x - 3.0, sy + 11.0, cz_cable - 1.0), (L_in - sma_in - 8.0, sy + 4.0, sz + 5.0),
               (L_in - sma_in - 4.5, sy + 0.4, sz + 1.0), (L_in - sma_in, sy, sz)]
        path = cq.Workplane("XY").spline(pts, tangents=[(0, -1, 0), (1, 0, 0)], includeCurrent=False)
        M["pigtail_coax"] = (cq.Workplane("XZ", origin=pts[0]).circle(1.13 / 2).sweep(path), "coax")

    # ---- toggle switch with silicone boot (-X wall) -------------------------------------
    wy, wz = L["sw_y"], P["sw_z"]
    M["switch_body"] = (rbox(0, wy - 6.6, wz - 3.95, 9.3, wy + 6.6, wz + 3.95), "pcb_blue")
    M["switch_bushing"] = (cyl_x(-w - 4.0, 0, wy, wz, 6.0), "metal")
    term = None
    for dy in (-4.7, 0.0, 4.7):
        t = rbox(9.3, wy + dy - 1.0, wz - 0.4, 13.3, wy + dy + 1.0, wz + 0.4)
        term = t if term is None else term.union(t)
    M["switch_terminals"] = (term, "metal")
    boot = cyl_x(-w - 4.0, -w, wy, wz, 11.0).union(cone_x(-w - 4.0, -w - 17.0, wy, wz, 9.0, 4.5))
    M["switch_boot"] = (boot.cut(cyl_x(-w - 4.0, -w + 1, wy, wz, 6.0)), "rubber")

    # ---- GX12 charge socket with cap (-X wall) ---------------------------------------------
    gy_, gz = L["charge_y"], P["charge_z"]
    gx12 = cyl_x(-w - 2.0, -w, gy_, gz, 15.0)                               # front flange
    gx12 = gx12.union(cyl_x(-w - 9.0, 2.5, gy_, gz, 12.0))                  # threaded barrel
    gx12 = gx12.union(cyl_x(2.5, 13.0, gy_, gz, 10.5))                      # rear body
    M["gx12_socket"] = (gx12, "metal")
    M["gx12_nut"] = (hex_x(0, 2.5, gy_, gz, 14.0).cut(cyl_x(-1, 3.5, gy_, gz, 12.0)), "steel_dark")
    M["gx12_pins"] = (cyl_x(13.0, 16.0, gy_ - 2.0, gz, 1.0).union(cyl_x(13.0, 16.0, gy_ + 2.0, gz, 1.0)), "gold")
    M["gx12_cap"] = (cyl_x(-w - 11.0, -w - 2.0, gy_, gz, 14.5).cut(cyl_x(-w - 9.0, -w - 1.0, gy_, gz, 12.0)), "steel_dark")

    # ---- lid gasket (compressed cord) and screws ------------------------------------------------
    go, H = P["groove_offset"], L["H_in"]
    hw = P["cord_d"] / 2
    ring = cavity_ring(P, L, go + hw, go - hw, H - P["groove_depth"], H)
    M["gasket_cord"] = (ring, "gasket")

    zs = H + P["lid_t"] - P["lid_cbore_depth"]
    for i, (lx, ly) in enumerate(L["lugs"]):
        s = cyl_z(lx, ly, zs, zs + 1.65, 5.7).union(cyl_z(lx, ly, zs - 10.0, zs, 3.0))
        M[f"lid_screw_{i + 1}"] = (s.cut(cq.Workplane("XY", origin=(lx, ly, zs + 0.65)).polygon(6, 2.3).extrude(2)), "steel_dark")
    for i, (hx, hy) in enumerate(L["holes"]):
        s = cyl_z(hx, hy, zt, zt + 2.0, 5.6).union(cyl_z(hx, hy, zt - 6.0, zt, 3.0))
        M[f"board_screw_{i + 1}"] = (s, "steel_dark")

    # ---- status LEDs in the lid pockets -----------------------------------------------------------
    for (sx, sy), n, c in zip(L["status_leds"], ("pwr", "rtk"), ("led_green", "led_red")):
        ztop = L["H_in"] + P["lid_t"] - P["status_led_skin"]
        led = cyl_z(sx, sy, ztop - 4.4, ztop - 1.5, 3.0).union(
            cq.Workplane("XY").add(cq.Solid.makeSphere(1.5, cq.Vector(sx, sy, ztop - 1.5))))
        led = led.union(cyl_z(sx, sy, ztop - 5.4, ztop - 4.4, 3.9))
        M[f"status_led_{n}"] = (led, c)

    # ---- power wiring (see the README wiring diagram) ---------------------------------------------
    dm = 0.5 * (L["div"][0] + L["div"][1])        # wires run on top of the divider rib, under the PCB
    xl = 16.5                                     # lane between the jack/switch bodies and the module
    pad_x = px0 + 1.5                             # pad row at the -X end of the board: B-, B+, IN-, IN+ across it
    t_x = 13.3                                    # switch terminal tips
    M["wire_batt_pos_to_switch"] = (wire([
        (hx1, cy + 2, 1.0), (bx1 + 4.5, cy + 2, 1.0), (bx1 + 4.5, dm, 3.8), (xl, dm, 3.8),
        (xl, wy, 3.8), (xl, wy, wz), (t_x, wy, wz)]), "wire_red")
    M["wire_batt_neg_to_module"] = (wire([
        (hx1, cy - 2, 1.0), (bx1 + 6.5, cy - 2, 1.0), (bx1 + 6.5, dm, 5.5), (xl - 0.4, dm, 5.5),
        (xl - 0.4, py0 + 1.5, 5.5), (pad_x, py0 + 1.5, 5.5), (pad_x, py0 + 1.5, zq)]), "wire_black")
    M["wire_switch_to_module"] = (wire([
        (t_x, wy - 4.7, wz), (xl - 1.3, wy - 4.7, wz), (xl - 1.3, py0 + 5.0, 7.5),
        (pad_x, py0 + 5.0, 7.5), (pad_x, py0 + 5.0, zq)]), "wire_red")
    for n, c, dy, stub, dx in (("pos", "wire_red", 2.0, 17.5, 2.5), ("neg", "wire_black", -2.0, 19.5, 5.5)):
        M[f"wire_charge_{n}"] = (wire([
            (16.0, gy_ + dy, gz), (stub, gy_ + dy, gz), (pad_x + dx, py1 - 1.5, 10.0), (pad_x + dx, py1 - 1.5, zq)]), c)
    bk_x = L["pcb_box"][0] + 1.2                  # VUSB / GND pads: position on the breakout est.
    for n, c, s_ in (("5v", "wire_red", 1.0), ("gnd", "wire_black", -1.0)):
        by_ = L["row_c"] + s_ * 9.2               # outside the HC-05 carrier, inside the standoffs
        M[f"wire_out_{n}"] = (wire([
            (px1 - 1.5, mcy + s_ * 3.0, zq), (px1 - 1.5, mcy + s_ * 3.0, 4.5), (bk_x, by_, 4.5), (bk_x, by_, zb)]), c)

    # ---- pipe clamp on the back face -------------------------------------------------------------------
    if P["fix_holes"]:
        od = P["pipe_od"]
        D = clamp_dims(P, L, od)
        saddle, cap = make_pipe_clamp(P, L, od)
        M["clamp_saddle"] = (saddle, "clamp")
        M["clamp_cap"] = (cap, "clamp")
        x_top = D["xc"] + 150.0                   # top of the pole is the +X (antenna) end
        x_phone = D["xc"] - 190.0                 # phone holder below the box, on the far side of the pole from it
        M["pvc_pipe"] = (cyl_x(x_phone - 110, x_top, D["yc"], D["zc"], od)
                         .cut(cyl_x(x_phone - 111, x_top + 1, D["yc"], D["zc"], od - 4.0)), "pvc")

        def at_phone(shape):                      # mount frame (pipe axis Z, phone +X) -> pole along +X, phone towards -Z
            return shape.rotate((0, 0, 0), (0, 1, 0), 90).translate((x_phone, D["yc"], D["zc"]))
        br, pcap = make_phone_bracket(P, od)
        M["phone_bracket"] = (at_phone(br), "clamp")
        M["phone_clamp_cap"] = (at_phone(pcap), "clamp")
        M["phone_cradle"] = (at_phone(phone_place(P, make_phone_cradle(P))), "clamp")
        link, spacer = make_phone_link(P)
        for nm, shp, col in (("phone_jaw_right", make_phone_jaw(P, 1, -P["phone_clear"]), "base"),
                             ("phone_jaw_left", make_phone_jaw(P, -1, -P["phone_clear"]), "base"),
                             ("phone_tongue_right", make_phone_tongue(P, 1), "base"),
                             ("phone_tongue_left", make_phone_tongue(P, -1), "base"),
                             ("phone_link", link, "metal"), ("phone_link_spacer", spacer, "metal")):
            M[nm] = (at_phone(phone_place(P, shp)), col)
        xb, t, c = P["phone_attach"], P["phone_plate_t"], P["phone_clear"]
        ph = rbox(xb - c - P["phone_l"], -P["phone_w"] / 2, t, xb - c, P["phone_w"] / 2, t + P["phone_t"] - 2.0, 9.0)
        M["phone"] = (at_phone(phone_place(P, ph)), "plastic_black")
        scr = rbox(xb - c - P["phone_l"] + 4, -P["phone_w"] / 2 + 3, t + P["phone_t"] - 2.0,
                   xb - c - 4, P["phone_w"] / 2 - 3, t + P["phone_t"] - 1.6, 7.0)
        M["phone_screen"] = (at_phone(phone_place(P, scr)), "pcb_blue")

        def on_pole(shape):                       # mount-local Z (pipe axis) -> enclosure +X at the pipe end
            return shape.rotate((0, 0, 0), (0, 1, 0), 90).translate((x_top, D["yc"], D["zc"]))
        tt, gt = P["ant_tray_t"], P["ant_gp_t"]
        al, aw, ah = P["ant_size"]
        if P["antenna"] == "survey":
            hp, top = P["srv_hex_h"], P["srv_hex_h"] + P["srv_top_t"]
            sd, sh = P["srv_size"]
            M["antenna_mount"] = (on_pole(make_survey_cap(P, od)), "clamp")
            bolt = cq.Workplane("XY", origin=(0, 0, 0.2)).polygon(6, 23.8 / 0.8660254).extrude(hp - 0.4)
            M["antenna_bolt_5_8"] = (on_pole(bolt.union(cyl_z(0, 0, hp - 0.2, top + 14.0, 15.9))), "metal")
            body = cyl_z(0, 0, top, top + 18.0, 46.0).union(cyl_z(0, 0, top + 18.0, top + 40.0, sd))
            body = body.union(cyl_z(0, 0, top + 40.0, top + sh, sd - 30.0).faces(">Z").edges().fillet(15.0))
            M["antenna_survey_k700"] = (on_pole(body.cut(cyl_z(0, 0, top - 1, top + 15.0, 16.0))), "plastic_white")
        elif P["antenna"] == "helical":
            top = P["hel_chamber_h"] + P["hel_top_t"]
            hd, hh = P["hel_size"]
            M["antenna_mount"] = (on_pole(make_helical_cap(P, od)), "clamp")
            sma = cyl_z(0, 0, top - 12.0, top + 9.0, 6.35).union(cyl_z(0, 0, top - 6.0, top - 4.0 - 0.01, 9.0))
            M["antenna_sma_bulkhead"] = (on_pole(sma.cut(cyl_z(0, 0, top + 3.0, top + 10.0, 4.2))), "gold")
            nut = cq.Workplane("XY", origin=(0, 0, top)).polygon(6, 8.0 / 0.8660254).extrude(2.2)
            M["antenna_sma_nut"] = (on_pole(nut.cut(cyl_z(0, 0, top - 1, top + 3, 6.4))), "gold")
            ant = cyl_z(0, 0, top + 2.2, top + 10.0, 9.5).cut(cyl_z(0, 0, top + 2.0, top + 9.2, 6.5))
            ant = ant.union(cyl_z(0, 0, top + 10.0, top + 10.0 + hh, hd).faces(">Z").edges().fillet(8.0))
            M["antenna_helical"] = (on_pole(ant), "plastic_white")
        else:
            M["antenna_mount"] = (on_pole(make_antenna_mount(P, od)), "clamp")
            M["antenna_ground_plane"] = (on_pole(cyl_z(0, 0, tt, tt + gt, P["ant_gp_d"])), "metal")
            M["antenna_ann_mb"] = (on_pole(rbox(-al / 2, -aw / 2, tt + gt, al / 2, aw / 2, tt + gt + ah, 12.0)
                                           .faces(">Z").edges().fillet(6.0)), "plastic_black")
        zh = D["z_cap"] - P["clamp_plate_t"]
        for i, (bx, by) in enumerate(D["bolts"]):
            M[f"clamp_bolt_{i + 1}"] = (cyl_z(bx, by, zh - 4.0, zh, 7.0).union(cyl_z(bx, by, zh, zh + 20.0, 4.0)), "steel_dark")
            nut = cq.Workplane("XY", origin=(bx, by, D["nut_seat"])).polygon(6, 7.0 / 0.8660254).extrude(3.2)
            M[f"clamp_nut_{i + 1}"] = (nut.cut(cyl_z(bx, by, D["nut_seat"] - 1, D["nut_seat"] + 5, 4.0)), "metal")
        zs0 = D["top"] - P["clamp_plate_t"] + P["clamp_cbore_depth"]
        for i, (fx, fy) in enumerate(L["fix"]):
            M[f"clamp_screw_{i + 1}"] = (cyl_z(fx, fy, zs0 - 2.0, zs0, 5.6).union(cyl_z(fx, fy, zs0, zs0 + 10.0, 3.0)), "steel_dark")
    return M


# Parts that are meant to bite into the print (thread-forming screws).
FASTENERS = ("lid_screw_", "board_screw_", "clamp_screw_", "clamp_bolt_")


def check(P, L, base, lid, M):
    print("\n=== Mock-up check (volume intersecting the printed parts, mm^3) ===")
    ok = True
    names = [n for n in M if not n.startswith(FASTENERS)]
    for n in names:
        vb, vl = vol(base.intersect(M[n][0])), vol(lid.intersect(M[n][0]))
        bb = M[n][0].val().BoundingBox()
        flag = "ok " if (vb + vl) < 0.01 else "CLASH"
        ok &= flag == "ok "
        print(f"  {flag}  {n:20s} base {vb:7.2f}  lid {vl:7.2f}   Z {bb.zmin:5.1f}..{bb.zmax:5.1f}")
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            v = vol(M[a][0].intersect(M[b][0]))
            if v > 0.01:
                ok = False
                print(f"  CLASH  {a} <-> {b}: {v:.2f}")
    top = max(M[n][0].val().BoundingBox().zmax for n in names if n != "gasket_cord")
    print(f"  tallest internal part reaches Z = {top:.1f}; lid underside is at Z = {L['H_in']:.1f}, "
          f"lip comes down to Z = {L['H_in'] - P['lip_h']:.1f} around the rim")
    print("  mock-ups: " + ("no clashes" if ok else "see above"))
    return ok


def build_assembly(base, lid, M):
    def col(k):
        return cq.Color(*COLORS[k], 1.0)
    assy = cq.Assembly(name="rtk_rover")
    assy.add(base, name="enclosure_base", color=col("base"))
    assy.add(lid, name="enclosure_lid", color=col("lid"))
    for n, (shape, c) in M.items():
        assy.add(shape, name=n, color=col(c))
    return assy


def main():
    if len(sys.argv) > 1:
        P["antenna"] = sys.argv[1]             # e.g.  python3 rtk_assembly.py helical
    L = layout(P)
    base, lid = make_base(P, L), make_lid(P, L)
    M = make_mockups(P, L)
    ok = check(P, L, base, lid, M)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), P["out_dir"])
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "rtk_rover_assembly.step" if P["antenna"] == "helical" else f"rtk_rover_assembly_{P['antenna']}.step")
    build_assembly(base, lid, M).export(path)
    print(f"\nwrote {path}  ({len(M) + 2} parts)")
    if not ok:
        print("WARNING: clashes reported above")
        sys.exit(1)


if __name__ == "__main__":
    main()
