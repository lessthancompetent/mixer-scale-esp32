"""
Shaded PNG renders of out/rtk_rover_assembly.step. Runs inside the FreeCAD GUI
(1.0 or newer), because the 3D view does the rendering:

    FreeCAD  ->  Macro  ->  Macros...  ->  Execute  this file
    or       FreeCAD.exe render_freecad.py

Writes out/render_*.png. Run rtk_assembly.py first.
"""
import os

import FreeCAD
import FreeCADGui
import ImportGui      # ImportGui (not Import) carries the STEP colours into the view

HERE = globals().get("HERE") or os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
SIZE = (1600, 1200)
LID_LIFT, SCREW_LIFT = 30.0, 48.0

DOC = "RTK_Render"
if DOC in FreeCAD.listDocuments():
    FreeCAD.closeDocument(DOC)
doc = FreeCAD.newDocument(DOC)
ImportGui.insert(os.path.join(OUT, "rtk_rover_assembly.step"), DOC)
doc.recompute()
view = FreeCADGui.getDocument(DOC).ActiveView

shapes = [o for o in doc.Objects if o.isDerivedFrom("Part::Feature")]
for o in doc.Objects:
    if o.TypeId == "App::Origin":
        o.ViewObject.Visibility = False
for o in shapes:
    o.ViewObject.DisplayMode = "Shaded"
    o.ViewObject.Deviation = 0.05
    o.ViewObject.AngularDeflection = 5.0


def named(*prefixes):
    return [o for o in shapes if o.Label.startswith(prefixes)]


lid, lid_screws = named("enclosure_lid"), named("lid_screw_")
home = {o.Name: o.Placement for o in lid + lid_screws}


def state(lid_on=True, lift=False, lid_alpha=0):
    for o in lid + lid_screws:
        o.ViewObject.Visibility = lid_on
        o.Placement = home[o.Name]
        if lift:
            p = FreeCAD.Placement(home[o.Name])
            p.Base = p.Base + FreeCAD.Vector(0, 0, SCREW_LIFT if o in lid_screws else LID_LIFT)
            o.Placement = p
    for o in lid:
        o.ViewObject.Transparency = lid_alpha


def shot(name, direction):
    """direction = the way the camera looks, in model coordinates."""
    view.setCameraType("Orthographic")
    if direction == "top":
        view.viewTop()
    else:
        # camera looks along its local -Z with local +Y up; keep model +Z up
        z = FreeCAD.Vector(*direction).negative().normalize()
        x = FreeCAD.Vector(0, 0, 1).cross(z).normalize()
        view.setCameraOrientation(FreeCAD.Rotation(x, z.cross(x), z, "ZXY"))
    view.fitAll()
    FreeCADGui.updateGui()
    path = os.path.join(OUT, name)
    view.saveImage(path, SIZE[0], SIZE[1], "White")
    print("wrote", path)


# +X is the antenna (SMA) end, -X the switch / charge-jack end, battery along -Y.
state(lid_on=True)
shot("render_closed_sma_end.png", (-1.0, 0.8, -0.7))
shot("render_closed_switch_end.png", (1.0, 0.8, -0.7))
state(lid_on=True, lid_alpha=75)
shot("render_lid_transparent.png", (-1.0, 0.8, -0.7))
state(lid_on=False)
shot("render_open_sma_end.png", (-1.0, 0.8, -1.1))
shot("render_open_switch_end.png", (1.0, -0.8, -1.1))
shot("render_open_top.png", "top")
state(lid_on=True, lift=True)
shot("render_exploded.png", (-1.0, 0.8, -0.6))
state(lid_on=True)
