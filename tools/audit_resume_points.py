"""Every call's return address must be a registered resume point (else a thread switched out below that call cannot
come back: the crashes of 2026-11-22). Run after 'build.py generate':  python -I tools/audit_resume_points.py"""
import bisect, csv, re, struct
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
reg = set(int(m.group(1), 16) for m in re.finditer(r'; // 0x([0-9a-f]+)', (ROOT / 'generated' / 'register_functions.cpp').read_text()))
d = (ROOT / 'game' / 'SLPS_252.17').read_bytes()
phoff, = struct.unpack_from('<I', d, 28); _, off, vaddr, *_ = struct.unpack_from('<8I', d, phoff)
rows = [r for r in csv.DictReader(open(ROOT / 'recomp' / 'sta_functions.csv')) if not r['Name'].startswith('entry_')]
bound = {int(r['Address'], 16) for r in csv.DictReader(open(ROOT / 'recomp' / 'sta_hle_bindings.csv'))}
calls = 0; missing = []
for r in rows:
    s, e = int(r['Start'], 16), int(r['End'], 16)
    if s in bound or s >= 0x1EDE00: continue
    for a in range(s, e, 4):
        w = struct.unpack_from('<I', d, off + a - vaddr)[0]
        if w >> 26 == 3 or (w >> 26 == 0 and (w & 0x3F) == 9):
            calls += 1
            if a + 8 not in reg: missing.append((a + 8, r['Name']))
print(f'{calls} calls; {len(missing)} return addresses not registered')
for a, n in missing[:20]: print(f'  0x{a:08x} in {n}')
