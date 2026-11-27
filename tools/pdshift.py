"""pdshift.py <present dump folder> [first last]: the signed sideways shift (pixels, half size search) of every
presented picture from the one before. A turn one way must not show steps the other way."""
import sys, os, re
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import pngread
D = sys.argv[1]
files = {}
for f in os.listdir(D):
    m = re.match(r'present-(\d+)(?:-id(\d+))?', f)
    if m:
        files[int(m.group(1))] = (f, m.group(2) or '-')
first = int(sys.argv[2]) if len(sys.argv) > 3 else min(files)
last = int(sys.argv[3]) if len(sys.argv) > 3 else max(files)
before = None
out = []
most = 110
for n in range(first, last + 1):
    if n not in files:
        continue
    p = np.asarray(pngread.load(os.path.join(D, files[n][0])))[::2, ::2, :3].astype(np.float32).mean(axis=2)[60:200]
    if before is not None and before.shape == p.shape:
        best, where = 1e18, 0
        for s in range(-most, most + 1):
            d = float(np.abs(before[:, most + s: before.shape[1] - most + s] - p[:, most: p.shape[1] - most]).mean())
            if d < best:
                best, where = d, s
        out.append(f"{files[n][1]}:{where * 2:+d}")
    before = p
print(' '.join(out))
