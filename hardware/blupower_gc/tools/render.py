#!/usr/bin/env python3
"""Composite per-layer SVG exports into realistic-ish PCB previews."""
import subprocess, os
from PIL import Image, ImageChops

PCB = "BluPower_GC.kicad_pcb"
OUT = "out/render"
os.makedirs(OUT, exist_ok=True)
W = 1800

def layer(name, mirror=False):
    svg = f"{OUT}/{name.replace('.', '_')}{'_m' if mirror else ''}.svg"
    cmd = ["kicad-cli", "pcb", "export", "svg", "--layers", name + ",Edge.Cuts", "--page-size-mode", "2",
           "--exclude-drawing-sheet", "--black-and-white", "--drill-shape-opt", "2", "-o", svg, PCB]
    if mirror:
        cmd.insert(4, "--mirror")
    subprocess.run(cmd, check=True, capture_output=True)
    png = svg[:-4] + ".png"
    subprocess.run(["rsvg-convert", "-w", str(W), "-b", "white", svg, "-o", png], check=True)
    im = Image.open(png).convert("L")
    return ImageChops.invert(im)  # white = drawn

def edge(mirror):
    svg = f"{OUT}/edge{'_m' if mirror else ''}.svg"
    cmd = ["kicad-cli", "pcb", "export", "svg", "--layers", "Edge.Cuts", "--page-size-mode", "2",
           "--exclude-drawing-sheet", "--black-and-white", "-o", svg, PCB]
    if mirror:
        cmd.insert(4, "--mirror")
    subprocess.run(cmd, check=True, capture_output=True)
    png = svg[:-4] + ".png"
    subprocess.run(["rsvg-convert", "-w", str(W), "-b", "white", svg, "-o", png], check=True)
    return ImageChops.invert(Image.open(png).convert("L"))

def compose(side):
    m = side == "bot"
    p = "B" if m else "F"
    e = edge(m)
    cu = ImageChops.subtract(layer(f"{p}.Cu", m), e)
    mask = ImageChops.subtract(layer(f"{p}.Mask", m), e)   # openings
    silk = ImageChops.subtract(layer(f"{p}.Silkscreen", m), e)
    size = cu.size
    img = Image.new("RGB", size, (20, 20, 20))
    board = Image.new("RGB", size, (18, 90, 45))
    # board area: fill inside edge using flood from center
    from PIL import ImageDraw
    bb = e.getbbox()
    area = Image.new("L", size, 0)
    ImageDraw.Draw(area).rectangle(bb, fill=255)
    img.paste(board, (0, 0), area)
    img.paste(Image.new("RGB", size, (40, 140, 70)), (0, 0), cu)          # copper under mask
    img.paste(Image.new("RGB", size, (215, 180, 90)), (0, 0), ImageChops.multiply(cu, mask))  # exposed copper (ENIG-ish)
    img.paste(Image.new("RGB", size, (240, 240, 240)), (0, 0), silk)
    img.paste(Image.new("RGB", size, (200, 200, 200)), (0, 0), e)
    img = img.crop((bb[0] - 20, bb[1] - 20, bb[2] + 20, bb[3] + 20))
    img.save(f"out/preview_{side}.png")

compose("top")
compose("bot")
print("ok")

# assembly view: preview + F.Fab (reference designators) in yellow
from PIL import ImageDraw
e = edge(False); bb = e.getbbox()
fab = ImageChops.subtract(layer("F.Fab"), e)
base = Image.open("out/preview_top.png")
fab = fab.crop((bb[0] - 20, bb[1] - 20, bb[2] + 20, bb[3] + 20))
base.paste(Image.new("RGB", base.size, (255, 230, 60)), (0, 0), fab)
base.save("out/preview_top_assembly.png")
