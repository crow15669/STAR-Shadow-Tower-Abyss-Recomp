"""ringblink.py <F1 folder>: for every picture n, per tile (40x40 at 480x360): how much more n differs from n-1 and
n+1 than those two differ from each other. A blink = a tile where min(d(n-1,n), d(n,n+1)) is far above d(n-1,n+1)."""
import sys, os, re
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngread

D = sys.argv[1]
files = {}
for f in os.listdir(D):
    m = re.match(r'(\d+)-([kv])(\d+)\.png', f)
    if m:
        files[int(m.group(1))] = (f, int(m.group(3)))
ns = sorted(files)
imgs = {}


def load(n):
    if n not in imgs:
        imgs[n] = np.asarray(pngread.load(os.path.join(D, files[n][0])))[..., :3].astype(np.float32).mean(axis=2)
    return imgs[n]


T = 40


def tiles(a):
    h, w = a.shape
    return a[:h // T * T, :w // T * T].reshape(h // T, T, w // T, T).mean(axis=(1, 3))


out = []
for i in range(1, len(ns) - 1):
    a, b, c = load(ns[i - 1]), load(ns[i]), load(ns[i + 1])
    dab = tiles(np.abs(a - b)); dbc = tiles(np.abs(b - c)); dac = tiles(np.abs(a - c))
    score = np.minimum(dab, dbc) - dac
    y, x = np.unravel_index(score.argmax(), score.shape)
    out.append((score.max(), ns[i], files[ns[i]][1], x, y, (score > 6).sum(), dab.mean(), dbc.mean(), dac.mean()))
    imgs.pop(ns[i - 1], None)
for s, n, k, x, y, cnt, dab, dbc, dac in sorted(out, reverse=True)[:40]:
    print(f"{n:3d} k{k}  score {s:6.2f} at tile {x},{y}  tiles>6: {cnt:3d}   mean d(prev) {dab:5.2f} d(next) {dbc:5.2f} d(prev,next) {dac:5.2f}")
