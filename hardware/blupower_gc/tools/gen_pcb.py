#!/usr/bin/python3
"""Build BluPower_GC.kicad_pcb with the KiCad 7 pcbnew API."""
import json, os, pcbnew
from pcbnew import VECTOR2I, FromMM

HERE = os.path.dirname(os.path.abspath(__file__))
PRJ = os.path.dirname(HERE)
OUT = os.path.join(PRJ, "BluPower_GC.kicad_pcb")
FPLIB = "/usr/share/kicad/footprints"

parts = json.load(open(os.path.join(HERE, "parts.json")))
uuids = json.load(open(os.path.join(HERE, "sym_uuids.json")))

OX, OY = 100.0, 100.0          # board origin (top-left) in mm
W, H = 20.32, 15.24             # board size
DY = 1.27                       # circuit sits 1.27 mm below the top edge (room for silk refs)

def P(x, y):                    # circuit coordinates
    return VECTOR2I(FromMM(OX + x), FromMM(OY + DY + y))

def P0(x, y):                   # absolute board coordinates (outline, silkscreen)
    return VECTOR2I(FromMM(OX + x), FromMM(OY + y))

board = pcbnew.NewBoard(OUT)
ds = board.GetDesignSettings()
ds.SetCopperLayerCount(2)
ds.m_TrackMinWidth = FromMM(0.2)
ds.m_ViasMinSize = FromMM(0.6)
ds.m_MinThroughDrill = FromMM(0.3)
ds.m_CopperEdgeClearance = FromMM(0.3)
ds.m_MinClearance = FromMM(0.15)
ds.SetAuxOrigin(P0(0, 0))
ds.SetGridOrigin(P0(0, 0))
nc = ds.m_NetSettings.m_DefaultNetClass
nc.SetClearance(FromMM(0.2))
nc.SetTrackWidth(FromMM(0.25))
nc.SetViaDiameter(FromMM(0.6))
nc.SetViaDrill(FromMM(0.3))

# ------------------------------------------------------------------ nets
NETS = {}
for n in ["GND", "VBAT", "3V3", "RUMBLE", "GATE", "MOT-"]:
    ni = pcbnew.NETINFO_ITEM(board, n if n == "GND" else "/" + n)
    board.Add(ni)
    NETS[n] = ni

# --------------------------------------------------------------- placement
# ref: (x, y, rotation_deg)   rotation is CCW as shown in pcbnew
PLACE = {
    "J1": (1.905, 3.81, 0),
    "C1": (4.20, 1.00, 0),
    "U1": (7.40, 4.20, 0),
    "C2": (10.10, 1.00, 0),
    "C3": (14.20, 1.45, 0),
    "J2": (18.415, 3.81, 0),
    "R1": (14.90, 5.30, 180),
    "Q1": (12.60, 8.20, 180),
    "R2": (15.60, 9.60, 270),
    "D1": (8.20, 8.20, 0),
    "J3": (6.35, 11.80, 90),
}
# silkscreen reference positions, absolute board coordinates (mm)
REFPOS = {
    "J1": (1.905, 0.75), "C1": (4.20, 0.75), "U1": (7.40, 0.75), "C2": (10.10, 0.75),
    "C3": (14.20, 0.75), "J2": (18.415, 0.75),
    "D1": (8.20, 7.95), "R1": (14.90, 5.00),
    "J3": (3.00, 13.07), "Q1": (12.60, 14.45), "R2": (15.60, 14.45),
}
FP = {}
for ref, info in parts.items():
    lib, name = info["fp"].split(":")
    fp = pcbnew.FootprintLoad(os.path.join(FPLIB, lib + ".pretty"), name)
    fp.SetReference(ref)
    fp.SetValue(info["val"])
    fp.SetFPIDAsString("BluPower_GC:" + name)   # project-local library copy
    fp.SetPath(pcbnew.KIID_PATH("/" + uuids[ref]))
    for k, v in info["extra"].items():
        if k != "DNP":
            fp.SetProperty(k, v)
    x, y, r = PLACE[ref]
    fp.SetPosition(P(x, y))
    fp.SetOrientationDegrees(r)
    rx, ry = REFPOS[ref]
    rt = fp.Reference()
    rt.SetLayer(pcbnew.F_SilkS)
    rt.SetVisible(True)
    rt.SetTextSize(VECTOR2I(FromMM(0.8), FromMM(0.8)))
    rt.SetTextThickness(FromMM(0.13))
    rt.SetPosition(P0(rx, ry))
    rt.SetTextAngleDegrees((360 - r) % 360)   # FP text angle is relative to the footprint: keep refs horizontal
    fp.Value().SetVisible(False)
    for pad in fp.Pads():
        n = info["nets"].get(pad.GetNumber())
        if n:
            pad.SetNet(NETS[n])
    board.Add(fp)
    FP[ref] = fp

def pad(ref, num):
    for p in FP[ref].Pads():
        if p.GetNumber() == num:
            q = p.GetPosition()
            return (pcbnew.ToMM(q.x) - OX, pcbnew.ToMM(q.y) - OY - DY)
    raise KeyError(ref + num)

# ------------------------------------------------------------------ copper
F, B = pcbnew.F_Cu, pcbnew.B_Cu
def track(net, pts, layer=F, w=0.25):
    pts = [pad(*p) if isinstance(p[0], str) else p for p in pts]
    for a, b in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(P(*a)); t.SetEnd(P(*b))
        t.SetWidth(FromMM(w)); t.SetLayer(layer); t.SetNet(NETS[net])
        board.Add(t)

def via(net, x, y):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(P(x, y)); v.SetDrill(FromMM(0.3)); v.SetWidth(FromMM(0.6))
    v.SetViaType(pcbnew.VIATYPE_THROUGH); v.SetNet(NETS[net])
    board.Add(v)

# VBAT (battery, from the TP4056 OUT+)
track("VBAT", [("J1", "1"), (2.90, 3.25), ("U1", "1")], w=0.4)
track("VBAT", [("C1", "1"), (3.25, 3.25)], w=0.4)
track("VBAT", [(4.60, 5.50), (5.30, 5.15), ("U1", "3")], w=0.3)          # EN tied to VIN
via("VBAT", 4.60, 5.50)
track("VBAT", [("J1", "1"), (3.50, 3.81), (3.50, 5.50), (4.60, 5.50)], layer=B, w=0.4)
track("VBAT", [(4.60, 5.50), (4.60, 9.40), ("J3", "1")], layer=B, w=0.5)   # motor supply
track("VBAT", [("D1", "1"), (6.35, 9.30), ("J3", "1")], w=0.4)
# 3V3
track("3V3", [("U1", "5"), (12.725, 3.25), (17.00, 3.25), ("J2", "1")], w=0.5)
track("3V3", [("C2", "1"), (9.15, 3.25)], w=0.5)
track("3V3", [("C3", "1"), (12.725, 3.25)], w=0.5)
# GND escapes
track("GND", [("U1", "2"), (4.60, 4.20)], w=0.3)
via("GND", 4.60, 4.20)
track("GND", [("C1", "2"), (6.10, 1.00)], w=0.4)
via("GND", 6.10, 1.00)
track("GND", [("Q1", "2"), (12.40, 6.20)], w=0.4)
via("GND", 12.40, 6.20)
track("GND", [("R2", "2"), (17.20, 10.425), ("J2", "3")], w=0.3)
# rumble: GPIO23 -> R1 -> gate, R2 pull-down, Q1 low-side switch, D1 flyback
track("RUMBLE", [("J2", "2"), ("R1", "1")])
track("GATE", [("R1", "2"), (14.775, 6.20), (14.775, 8.60), (14.30, 9.15), ("Q1", "1")])
track("GATE", [(14.775, 8.60), ("R2", "1")])
track("MOT-", [("D1", "2"), ("Q1", "3")], w=0.4)
track("MOT-", [("D1", "2"), (9.85, 9.60), ("J3", "2")], w=0.4)
# stitching vias (GND pour both sides)
for x, y in [(1.10, 9.00), (11.60, 10.60), (17.20, 6.35 + 0.0) if False else (16.90, 12.00), (12.00, 5.00)]:
    via("GND", x, y)

# ------------------------------------------------------------------ outline
def seg(a, b, layer=pcbnew.Edge_Cuts, w=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(P0(*a)); s.SetEnd(P0(*b)); s.SetLayer(layer); s.SetWidth(FromMM(w))
    board.Add(s)
for a, b in [((0, 0), (W, 0)), ((W, 0), (W, H)), ((W, H), (0, H)), ((0, H), (0, 0))]:
    seg(a, b)

# ------------------------------------------------------------------ zones
def zone(layer, net):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer); z.SetNet(NETS[net])
    ol = z.Outline(); ol.NewOutline()
    for x, y in [(0, 0), (W, 0), (W, H), (0, H)]:
        ol.Append(FromMM(OX + x), FromMM(OY + y))
    z.SetLocalClearance(FromMM(0.25))
    z.SetMinThickness(FromMM(0.2))
    z.SetThermalReliefGap(FromMM(0.25))
    z.SetThermalReliefSpokeWidth(FromMM(0.3))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    z.SetAssignedPriority(0)
    board.Add(z)
zone(B, "GND")
zone(F, "GND")

# ------------------------------------------------------------------ text
def text(s, x, y, layer, size=0.8, mirror=False, just=None):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s); t.SetPosition(P0(x, y)); t.SetLayer(layer)
    t.SetTextSize(VECTOR2I(FromMM(size), FromMM(size)))
    t.SetTextThickness(FromMM(size * 0.15))
    if mirror:
        t.SetMirrored(True)
    if just is not None:
        t.SetHorizJustify(just)
    board.Add(t)

BS = pcbnew.B_SilkS
text("BluPower GC", 9.50, 3.00, BS, 0.9, True)
text("AP2112K  v1.0", 9.50, 4.40, BS, 0.8, True)
text("BAT+", 1.905, 3.81 - 2.00 + 1.27, BS, 0.8, True)
text("GND", 1.905, 6.35 + 1.27 + 1.60, BS, 0.8, True)
text("3V3", 15.80, 3.81 + 1.27, BS, 0.8, True)
text("IO23", 15.60, 6.35 + 1.27, BS, 0.8, True)
text("GND", 15.80, 8.89 + 1.27, BS, 0.8, True)
text("M+", 6.35, 11.80 + 1.27 - 1.65, BS, 0.8, True)
text("M-", 8.89, 11.80 + 1.27 - 1.65, BS, 0.8, True)

filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
board.Save(OUT)
print("saved", OUT)
for ref in PLACE:
    print(ref, [(p.GetNumber(), p.GetNetname(), tuple(round(v, 3) for v in pad(ref, p.GetNumber()))) for p in FP[ref].Pads()])
