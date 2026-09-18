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

# camera moves are animated by default and a screenshot can catch one half-way
_view_prefs = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
_anim = _view_prefs.GetBool("UseNavigationAnimations", True)
_view_prefs.SetBool("UseNavigationAnimations", False)

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
pipe = named("pvc_pipe", "antenna_")
cap_side = named("clamp_cap", "clamp_bolt_")            # comes off downwards in the exploded view
mount = named("clamp_", "pvc_pipe", "antenna_")
home = {o.Name: o.Placement for o in shapes}


def state(lid_on=True, mount_on=True, explode=False, lid_alpha=0):
    for o in shapes:
        o.Placement = home[o.Name]
    for o in lid + lid_screws:
        o.ViewObject.Visibility = lid_on
    for o in mount:
        o.ViewObject.Visibility = mount_on
    if explode:
        for group, dz in ((lid, LID_LIFT), (lid_screws, SCREW_LIFT), (pipe, -14.0), (cap_side, -32.0)):
            for o in group:
                p = FreeCAD.Placement(home[o.Name])
                p.Base = p.Base + FreeCAD.Vector(0, 0, dz)
                o.Placement = p
    for o in lid:
        o.ViewObject.Transparency = lid_alpha


def shot(name, direction):
    """direction = the way the camera looks, in model coordinates."""
    view.setCameraType("Orthographic")
    if direction == "top":
        view.setCameraOrientation(FreeCAD.Rotation())
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
state()
shot("render_closed_sma_end.png", (-1.0, 0.8, -0.7))
shot("render_closed_switch_end.png", (1.0, 0.8, -0.7))
shot("render_back_pipe_clamp.png", (-1.0, 0.7, 0.9))
shot("render_pole_antenna.png", (-0.6, 1.0, -0.5))
state(lid_alpha=75, mount_on=False)
shot("render_lid_transparent.png", (-1.0, 0.8, -0.7))
state(lid_on=False, mount_on=False)
shot("render_open_sma_end.png", (-1.0, 0.8, -1.1))
shot("render_open_switch_end.png", (1.0, -0.8, -1.1))
shot("render_open_top.png", "top")
state(explode=True)
shot("render_exploded.png", (-1.0, 0.8, -0.45))
state()
_view_prefs.SetBool("UseNavigationAnimations", _anim)

# helical-antenna variant (python3 rtk_assembly.py helical), pole view only
HEL = os.path.join(OUT, "rtk_rover_assembly_helical.step")
if os.path.exists(HEL):
    _view_prefs.SetBool("UseNavigationAnimations", False)
    if DOC + "_helical" in FreeCAD.listDocuments():
        FreeCAD.closeDocument(DOC + "_helical")
    hdoc = FreeCAD.newDocument(DOC + "_helical")
    ImportGui.insert(HEL, hdoc.Name)
    hdoc.recompute()
    for o in hdoc.Objects:
        if o.TypeId == "App::Origin":
            o.ViewObject.Visibility = False
        elif o.isDerivedFrom("Part::Feature"):
            o.ViewObject.DisplayMode = "Shaded"
    view = FreeCADGui.getDocument(hdoc.Name).ActiveView
    shot("render_pole_helical.png", (-0.6, 1.0, -0.5))
    _view_prefs.SetBool("UseNavigationAnimations", _anim)
