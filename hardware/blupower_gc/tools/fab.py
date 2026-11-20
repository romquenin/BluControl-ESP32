#!/usr/bin/env python3
"""Gerbers/drill via kicad-cli, then JLCPCB BOM + CPL (3V3 and 5V variants)."""
import subprocess, csv, json, os, shutil, zipfile
PCB = "BluPower_GC.kicad_pcb"
parts = json.load(open("tools/parts.json"))
shutil.rmtree("fab", ignore_errors=True)
os.makedirs("fab/gerbers")
run = lambda *a: subprocess.run(a, check=True, capture_output=True)
run("kicad-cli", "pcb", "export", "gerbers", "--layers",
    "F.Cu,B.Cu,F.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts",
    "--subtract-soldermask", "--no-x2", "--use-drill-file-origin", "-o", "fab/gerbers/", PCB)
run("kicad-cli", "pcb", "export", "drill", "--format", "excellon", "--drill-origin", "plot",
    "--excellon-units", "mm", "-o", "fab/gerbers/", PCB)
run("kicad-cli", "pcb", "export", "pos", "--side", "front", "--format", "csv", "--units", "mm",
    "--use-drill-file-origin", "-o", "fab/pos_raw.csv", PCB)
for f in os.listdir("fab/gerbers"):
    if f.endswith(".gbrjob"):
        os.remove(os.path.join("fab/gerbers", f))
with zipfile.ZipFile("fab/BluPower_GC_gerbers_JLCPCB.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir("fab/gerbers")):
        z.write(os.path.join("fab/gerbers", f), f)

SMD = [r for r in parts if not r.startswith("J")]
# CPL: JLC wants Designator, Mid X, Mid Y, Layer, Rotation
rows = list(csv.DictReader(open("fab/pos_raw.csv")))
with open("fab/CPL_JLCPCB.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for r in rows:
        if r["Ref"] in SMD:
            rot = float(r["Rot"]) % 360
            w.writerow([r["Ref"], f'{float(r["PosX"]):.3f}mm', f'{float(r["PosY"]):.3f}mm', "Top", f"{rot:.0f}"])

def bom(name, include_r4):
    groups = {}
    for ref in SMD:
        p = parts[ref]
        if ref == "R4" and not include_r4:
            continue
        key = (p["val"], p["fp"].split(":")[1], p["extra"].get("LCSC", ""))
        groups.setdefault(key, []).append(ref)
    with open(f"fab/{name}", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (val, fp, lcsc), refs in sorted(groups.items(), key=lambda kv: kv[1][0]):
            w.writerow([val, ",".join(sorted(refs)), fp, lcsc])
bom("BOM_JLCPCB.csv", True)
os.remove("fab/pos_raw.csv")
print(open("fab/CPL_JLCPCB.csv").read()); print(open("fab/BOM_JLCPCB.csv").read())
print(sorted(os.listdir("fab/gerbers")))
