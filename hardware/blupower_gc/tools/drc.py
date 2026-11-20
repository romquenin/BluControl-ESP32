#!/usr/bin/python3
import pcbnew, re, sys, collections
b = pcbnew.LoadBoard(sys.argv[1] if len(sys.argv) > 1 else 'BluPower_GC.kicad_pcb')
pcbnew.WriteDRCReport(b, 'out/drc.rpt', pcbnew.EDA_UNITS_MILLIMETRES, True)
t = open('out/drc.rpt').read()
blocks = re.split(r'\n(?=\[)', t)
c = collections.Counter()
for bl in blocks[1:]:
    k = bl.split(']')[0][1:]
    sev = re.search(r'Severity: (\w+)', bl)
    c[(k, sev.group(1) if sev else '?')] += 1
    if k not in ('lib_footprint_issues', 'text_height', 'silk_overlap', 'silk_over_copper', 'silk_edge_clearance'):
        print(bl.strip()); print('--')
print(c)
print([l for l in t.splitlines() if 'unconnected' in l.lower()][:5])
