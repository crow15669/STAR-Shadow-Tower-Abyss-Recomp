"""backtest.py <log> <dump dir> [worst]: with STAR_SMOOTH_SELFTEST=1 and STAR_SMOOTH_TEST_T=0 the first picture of a
frame is the game's own and the others are passes drawn from the camera of the frame BEFORE, with the objects where
they were then. Such a pass must show what the frame before's own picture showed. Where it does not, the pass could
not draw something: a piece the game left out or cut to fit the frame's own camera, or a wrongly placed object.
Compares each pass with the own picture of the frame before (PS2X_PCSX2_PRESENT_DUMP files, ids from the log's
[selftest] lines) in 8x8 blocks, and lists the frames with the most blocks that differ clearly."""
import sys, os, re
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import pngread
log, D = sys.argv[1], sys.argv[2]
worst = int(sys.argv[3]) if len(sys.argv) > 3 else 10
files = {}
for f in os.listdir(D):
    m = re.search(r'-id(\d+)\.png$', f)
    if m:
        files[int(m.group(1))] = f
groups = []
for line in open(log, encoding='utf-8', errors='replace'):
    if line.startswith('[selftest]'):
        ids = []
        for x in line.split()[1:]:
            if not x.isdigit():
                break
            ids.append(int(x))
        if len(ids) > 1:
            groups.append(ids)
B = 8
def blocks(i):
    a = np.asarray(pngread.load(os.path.join(D, files[i])), dtype=np.float32).mean(axis=2)
    h, w = a.shape
    h -= h % B
    w -= w % B
    return a[:h, :w].reshape(h // B, B, w // B, B).mean(axis=(1, 3))
results = []
for before, now in zip(groups, groups[1:]):
    own, passes = before[0], [i for i in now[1:] if i in files]
    if own not in files or not passes or now[0] != before[-1] + 1:
        continue  # frames that do not follow each other (a still frame or a cut lay between)
    a, b = blocks(own), blocks(passes[0])
    d = np.abs(a - b)
    bad = d > 24
    n = int(bad.sum())
    ys, xs = np.nonzero(bad)
    results.append((n, own, passes[0], (int(xs.mean() * B), int(ys.mean() * B)) if n else None, float(d.mean())))
if not results:
    print('no pairs of following frames: check the dump range and the log')
    sys.exit(0)
results.sort(reverse=True)
counts = sorted(r[0] for r in results)
print(f'{len(results)} frames compared; blocks that differ clearly: median {counts[len(counts) // 2]}, most {counts[-1]}')
print('worst (blocks, own picture id of the frame before, pass id, where, mean difference):')
for r in results[:worst]:
    print('  ', r[0], r[1], r[2], r[3], f'{r[4]:.2f}')
