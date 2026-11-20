#!/usr/bin/env python3
"""Generate BluPower_GC.kicad_sch (KiCad 7 format).

Every pin gets a short wire stub ending in a net label / power symbol, so the
schematic stays readable without hand-routed wires.
"""
import re, uuid, os, json

LIB = "/usr/share/kicad/symbols"
OUT = os.path.join(os.path.dirname(__file__), "..", "BluPower_GC.kicad_sch")
PROJECT = "BluPower_GC"
ROOT_UUID = "3b9e5a10-2c4d-4e8f-8a1b-5c6d7e8f9a0b"

def U():
    return str(uuid.uuid4())

def extract(text, start):
    depth = 0
    for i in range(start, len(text)):
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise ValueError

def lib_symbol(libfile, name, libname):
    t = open(os.path.join(LIB, libfile)).read()
    i = t.index('(symbol "%s"' % name)
    s = extract(t, i)
    # rename top-level symbol to lib:name (children keep bare names)
    s = s.replace('(symbol "%s"' % name, '(symbol "%s:%s"' % (libname, name), 1)
    pins = {}
    for m in re.finditer(r'\(pin \w+ \w+ \(at ([-\d.]+) ([-\d.]+) (\d+)\) \(length ([\d.]+)\)[\s\S]*?\(number "(\w+)"', s):
        x, y, a, l, n = m.groups()
        pins[n] = (float(x), float(y), int(a))
    return s, pins

SYMS = {
    "Device:R": ("Device.kicad_sym", "R"),
    "Device:C": ("Device.kicad_sym", "C"),
    "Device:D_Schottky": ("Device.kicad_sym", "D_Schottky"),
    "Regulator_Linear:AP2204K-1.5": ("Regulator_Linear.kicad_sym", "AP2204K-1.5"),
    "Transistor_FET:BSS138": ("Transistor_FET.kicad_sym", "BSS138"),
    "Connector_Generic:Conn_01x02": ("Connector_Generic.kicad_sym", "Conn_01x02"),
    "Connector_Generic:Conn_01x03": ("Connector_Generic.kicad_sym", "Conn_01x03"),
    "power:GND": ("power.kicad_sym", "GND"),
}
LIBS, PINS = {}, {}
for k, (f, n) in SYMS.items():
    LIBS[k], PINS[k] = lib_symbol(f, n, k.split(":")[0])

# ---------------------------------------------------------------- parts
# ref, lib_id, value, footprint, x, y, {pin: net}, extra fields
C05 = "Capacitor_SMD:C_0805_2012Metric"
PARTS = [
    ("J1", "Connector_Generic:Conn_01x02", "Batterie (TP4056 OUT)", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
     35.56, 60.96, {"1": "VBAT", "2": "GND"}, {}),
    ("C1", "Device:C", "10u", C05, 60.96, 78.74, {"1": "VBAT", "2": "GND"}, {"LCSC": "C15850", "MPN": "CL21A106KAYNNNE"}),
    ("U1", "Regulator_Linear:AP2204K-1.5", "AP2112K-3.3", "Package_TO_SOT_SMD:SOT-23-5",
     76.2, 60.96, {"1": "VBAT", "2": "GND", "3": "VBAT", "5": "3V3"}, {"LCSC": "C51118", "MPN": "AP2112K-3.3TRG1"}),
    ("C2", "Device:C", "22u", C05, 96.52, 76.2, {"1": "3V3", "2": "GND"}, {"LCSC": "C45783", "MPN": "CL21A226MAQNNNE"}),
    ("C3", "Device:C", "100u", "Capacitor_SMD:C_1206_3216Metric", 106.68, 76.2, {"1": "3V3", "2": "GND"},
     {"LCSC": "C15008", "MPN": "CL31A107MQHNNNE"}),
    ("J2", "Connector_Generic:Conn_01x03", "ESP32", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
     129.54, 60.96, {"1": "3V3", "2": "RUMBLE", "3": "GND"}, {}),
    ("R1", "Device:R", "100", "Resistor_SMD:R_0805_2012Metric", 114.3, 101.6, {"1": "RUMBLE", "2": "GATE"},
     {"LCSC": "C17408", "MPN": "0805W8F1000T5E"}),
    ("R2", "Device:R", "10k", "Resistor_SMD:R_0805_2012Metric", 101.6, 116.84, {"1": "GATE", "2": "GND"},
     {"LCSC": "C17414", "MPN": "0805W8F1002T5E"}),
    ("Q1", "Transistor_FET:BSS138", "AO3400A", "Package_TO_SOT_SMD:SOT-23", 88.9, 101.6,
     {"1": "GATE", "2": "GND", "3": "MOT-"}, {"LCSC": "C20917", "MPN": "AO3400A"}),
    ("D1", "Device:D_Schottky", "B5819W", "Diode_SMD:D_SOD-123", 68.58, 91.44, {"1": "VBAT", "2": "MOT-"},
     {"LCSC": "C8598", "MPN": "B5819W"}),
    ("J3", "Connector_Generic:Conn_01x02", "Moteur", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
     50.8, 101.6, {"1": "VBAT", "2": "MOT-"}, {}),
]
MIRROR = {"J2"}  # connector drawn facing left

STUB = 2.54
items, wires, labels, pwr = [], [], [], []
sym_uuids = {}
pcount = [0]

def pin_abs(x, y, px, py, pa, mirror):
    if mirror:
        px = -px
        pa = {0: 180, 180: 0}.get(pa, pa)
    ax, ay = x + px, y - py
    # outward direction = opposite of pin direction angle
    d = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[pa]
    return ax, ay, d

def fmt(v):
    return ("%.2f" % v).rstrip("0").rstrip(".")

def power_sym(kind, x, y, rot=0):
    pcount[0] += 1
    ref = "#PWR%02d" % pcount[0]
    u = U()
    return f'''  (symbol (lib_id "power:{kind}") (at {fmt(x)} {fmt(y)} {rot}) (unit 1)
    (in_bom yes) (on_board yes) (dnp no) (uuid {u})
    (property "Reference" "{ref}" (at {fmt(x)} {fmt(y+3.81 if kind=='GND' else y-3.81)} 0)
      (effects (font (size 1.27 1.27)) hide))
    (property "Value" "{kind}" (at {fmt(x)} {fmt(y+3.3 if kind=='GND' else y-3.3)} 0)
      (effects (font (size 1.27 1.27))))
    (property "Footprint" "" (at {fmt(x)} {fmt(y)} 0) (effects (font (size 1.27 1.27)) hide))
    (property "Datasheet" "" (at {fmt(x)} {fmt(y)} 0) (effects (font (size 1.27 1.27)) hide))
    (pin "1" (uuid {U()}))
    (instances (project "{PROJECT}" (path "/{ROOT_UUID}" (reference "{ref}") (unit 1))))
  )'''

for ref, lib, val, fp, x, y, nets, extra in PARTS:
    su = U()
    sym_uuids[ref] = su
    mirror = ref in MIRROR
    dnp = extra.get("DNP") == "1"
    props = [
        ("Reference", ref, x + 2.54, y - 6.35 if lib != "Device:R" and lib != "Device:C" else y - 1.27, False),
        ("Value", val, x + 2.54, y + 6.35 if lib != "Device:R" and lib != "Device:C" else y + 1.27, False),
        ("Footprint", fp, x, y, True),
        ("Datasheet", "", x, y, True),
    ]
    for k, v in extra.items():
        if k != "DNP":
            props.append((k, v, x, y, True))
    ptxt = "\n".join(
        f'    (property "{k}" "{v}" (at {fmt(px)} {fmt(py)} 0) (effects (font (size 1.27 1.27)){" (justify left)" if not h else ""}{" hide" if h else ""}))'
        for k, v, px, py, h in props)
    pins_txt = "\n".join(f'    (pin "{n}" (uuid {U()}))' for n in PINS[lib])
    items.append(f'''  (symbol (lib_id "{lib}") (at {fmt(x)} {fmt(y)} 0){" (mirror y)" if mirror else ""} (unit 1)
    (in_bom yes) (on_board yes) (dnp {"yes" if dnp else "no"}) (uuid {su})
{ptxt}
{pins_txt}
    (instances (project "{PROJECT}" (path "/{ROOT_UUID}" (reference "{ref}") (unit 1))))
  )''')
    for pn, net in nets.items():
        px, py, pa = PINS[lib][pn]
        ax, ay, (dx, dy) = pin_abs(x, y, px, py, pa, mirror)
        ex, ey = ax + dx * STUB, ay + dy * STUB
        wires.append(f'  (wire (pts (xy {fmt(ax)} {fmt(ay)}) (xy {fmt(ex)} {fmt(ey)})) (stroke (width 0) (type default)) (uuid {U()}))')
        if net == "GND":
            if dy == -1:  # stub goes up: extend around? just place symbol and rotate
                pwr.append(power_sym("GND", ex, ey, 180))
            else:
                pwr.append(power_sym("GND", ex, ey, 0))
        elif net == "VCC":
            pwr.append(power_sym("VCC", ex, ey, 0 if dy != 1 else 180))
        else:
            ang = {(-1, 0): 180, (1, 0): 0, (0, -1): 90, (0, 1): 270}[(dx, dy)]
            just = "right" if ang == 180 else "left"
            if ang == 90:
                just = "left"
            if ang == 270:
                just = "right"
            labels.append(f'  (label "{net}" (at {fmt(ex)} {fmt(ey)} {ang}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid {U()}))')

notes = [
    ("BluPower GC - alimentation 3,3 V + vibration pour manette GameCube Bluetooth (ESP32)", 30.48, 25.4, 2.0),
    ("J1 = sortie du TP4056 (OUT+ / OUT-)   J2 = vers l'ESP32 (3V3, GND, GPIO23)   J3 = moteur de vibration", 30.48, 30.48, 1.27),
    ("U1 remplace l'AMS1117 de la carte ESP32 (a dessouder) : chute ~0,25 V a 600 mA au lieu de ~1,1 V", 30.48, 33.02, 1.27),
    ("Q1 commute le moteur depuis la batterie ; D1 = diode de roue libre ; R2 garde le moteur arrete au demarrage", 30.48, 35.56, 1.27),
]
texts = "\n".join(
    f'  (text "{t}" (at {fmt(x)} {fmt(y)} 0) (effects (font (size {s} {s})) (justify left bottom)) (uuid {U()}))'
    for t, x, y, s in notes)

libtxt = "\n".join("    " + LIBS[k].replace("\n", "\n  ") for k in SYMS)
doc = f'''(kicad_sch (version 20230121) (generator eeschema)
  (uuid {ROOT_UUID})
  (paper "A4")
  (title_block
    (title "BluPower GC")
    (date "2026-09-30")
    (rev "1.0")
    (comment 1 "LDO 3.3 V + rumble driver for ESP32 Bluetooth GameCube controller mod")
    (comment 2 "Replaces the AMS1117 of the ESP32 dev board")
  )
  (lib_symbols
{libtxt}
  )
{texts}
{chr(10).join(wires)}
{chr(10).join(labels)}
{chr(10).join(items)}
{chr(10).join(pwr)}
  (sheet_instances (path "/" (page "1")))
)
'''
LOCAL = "BluPower_GC"
REMAP = {"Device:R": "R", "Device:C": "C", "Device:D_Schottky": "D_Schottky",
         "Regulator_Linear:AP2204K-1.5": "AP2112K-3.3", "Transistor_FET:BSS138": "AO3400A",
         "Connector_Generic:Conn_01x02": "Conn_01x02", "Connector_Generic:Conn_01x03": "Conn_01x03",
         "power:GND": "GND"}
FPLIBS = ["Resistor_SMD:", "Capacitor_SMD:", "Package_TO_SOT_SMD:", "Connector_PinHeader_2.54mm:", "Diode_SMD:"]

def localize(txt):
    txt = txt.replace('"AP2204K-1.5', '"AP2112K-3.3').replace("Regulator_Linear:AP2204K-1.5", "Regulator_Linear:AP2112K-3.3")
    txt = txt.replace('"BSS138', '"AO3400A').replace("Transistor_FET:BSS138", "Transistor_FET:AO3400A")
    for k, v in REMAP.items():
        k2 = k.replace("AP2204K-1.5", "AP2112K-3.3").replace("BSS138", "AO3400A")
        txt = txt.replace('"%s"' % k2, '"%s:%s"' % (LOCAL, v))
    for l in FPLIBS:
        txt = txt.replace('"' + l, '"%s:' % LOCAL)
    return txt

doc = localize(doc)
# project-local symbol library (same symbols, bare names)
syms = []
for k, v in REMAP.items():
    body = LIBS[k].replace('(symbol "%s"' % k, '(symbol "%s"' % k.split(":")[1], 1)
    body = localize(body)
    syms.append("  " + body)
open(os.path.join(os.path.dirname(OUT), LOCAL + ".kicad_sym"), "w").write(
    "(kicad_symbol_lib (version 20220914) (generator kicad_symbol_editor)\n" + "\n".join(syms) + "\n)\n")
open(OUT, "w").write(doc)
json.dump(sym_uuids, open(os.path.join(os.path.dirname(__file__), "sym_uuids.json"), "w"), indent=1)
json.dump({p[0]: {"fp": p[3], "val": p[2], "nets": p[6], "extra": p[7]} for p in PARTS},
          open(os.path.join(os.path.dirname(__file__), "parts.json"), "w"), indent=1)
print("written", OUT)
