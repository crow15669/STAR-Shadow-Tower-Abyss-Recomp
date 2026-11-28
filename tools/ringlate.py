"""ringlate.py <F1 folder>: every picture whose hand-over gap is uneven or whose draw count is double, with its frame
(from frames.txt), its place in the frame and the frame's lines."""
import sys, os, re
D = sys.argv[1]
pics = []
for line in open(os.path.join(D, 'list.txt')):
    m = re.match(r'(\d+)-([kv])(\d+)\.png\s+([\d.]+) ms before the newest; draws (\d+) prims (\d+)', line)
    if m:
        pics.append(dict(n=int(m.group(1)), id=int(m.group(3)), t=float(m.group(4)), draws=int(m.group(5))))
where = {}
frames = {}
cur = None
for line in open(os.path.join(D, 'frames.txt'), errors='replace'):
    if line.startswith('F '):
        m = re.match(r'F frame (\d+) tick (\d+) stride (\d+).*?own run ([\d.]+) ms, a pass ([\d.]+) ms, pictures: (.*)', line)
        if not m:
            cur = None
            continue
        cur = int(m.group(1))
        ids = [(int(a), float(b)) for a, b in re.findall(r'(\d+)@([\d.]+)', m.group(6))]
        frames[cur] = dict(ids=ids, own=float(m.group(4)), lines=[])
        for k, (i, at) in enumerate(ids):
            where[i] = (cur, k, len(ids), at)
    elif cur is not None and line[:2] in ('C ', 'R ', 'W ', 'L '):
        frames[cur]['lines'].append(line.rstrip()[:230])
med = sorted(p['draws'] for p in pics)[len(pics) // 2]
print(f"{len(pics)} pictures, median draws {med}")
for a, b in zip(pics, pics[1:]):
    gap = a['t'] - b['t']
    odd = gap > 10.5 or gap < 3.5
    double = b['draws'] > 1.5 * med
    skipped = b['id'] - a['id'] - 1
    if odd or double or skipped:
        w = where.get(b['id'])
        print(f"{b['n']:3d} id {b['id']}  gap {gap:5.1f} ms  draws {b['draws']:3d}{'  SKIPPED ' + str(skipped) + ' before' if skipped else ''}"
              f"  frame {w[0] if w else '?'} picture {w[1] + 1 if w else '?'} of {w[2] if w else '?'} @{w[3] if w else 0:.3f}")
if '-v' in sys.argv:
    for f in sys.argv[sys.argv.index('-v') + 1:]:
        f = int(f)
        for g in (f - 1, f, f + 1):
            if g in frames:
                print(g, frames[g]['ids'], 'own', frames[g]['own'])
                for l in frames[g]['lines']:
                    print('   ', l)
