"""ringgun.py <F1 folder> [first last]: where the player's weapon (light grey, bottom middle) is in every recorded
picture: the centre of its pixels and how many, next to the picture's id. A weapon that jumps between pictures of one
game frame is the in-between placement, not the game's animation."""
import sys, os, re
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import pngread
D = sys.argv[1]
files = {}
for f in os.listdir(D):
    m = re.match(r'(\d+)-([kv])(\d+)\.png', f)
    if m:
        files[int(m.group(1))] = (f, int(m.group(3)))
first = int(sys.argv[2]) if len(sys.argv) > 3 else min(files)
last = int(sys.argv[3]) if len(sys.argv) > 3 else max(files)
for n in range(first, last + 1):
    if n not in files:
        continue
    a = np.asarray(pngread.load(os.path.join(D, files[n][0])))[..., :3].astype(np.int32)
    part = a[230:360, 180:340]
    lo, hi = part.min(axis=2), part.max(axis=2)
    mask = (lo > 110) & (hi - lo < 70)
    count = int(mask.sum())
    if count:
        ys, xs = np.nonzero(mask)
        print(f"{n:3d} id {files[n][1]}  x {xs.mean() + 180:6.1f}  y {ys.mean() + 230:6.1f}  pixels {count}")
    else:
        print(f"{n:3d} id {files[n][1]}  none")
