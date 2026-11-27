"""pdsteps.py <present dump folder> [first last]: mean difference of each presented picture from the one before
(quarter size), to find where the picture moves or breaks."""
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
for n in range(first, last + 1):
    if n not in files:
        continue
    p = np.asarray(pngread.load(os.path.join(D, files[n][0])))[::4, ::4, :3].astype(np.float32).mean(axis=2)
    if before is not None and before.shape == p.shape:
        print(f"{n:4d} id {files[n][1]:>5}  diff {float(np.abs(p - before).mean()):6.2f}")
    before = p
