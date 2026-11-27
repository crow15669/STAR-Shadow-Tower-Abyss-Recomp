"""flashpos.py <F1 folder>: the pictures with a muzzle flash (bright orange-yellow pixels in the middle of the picture)
and where its middle is; a flash that sits on the barrel keeps its place through a shot's pictures while the view turns."""
import sys, os, glob
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngread
out = []
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.png'))):
    a = pngread.load(f).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    m = (r > 225) & (g > 170) & (b < 200) & (r - b > 40)
    m[:140, :] = False; m[:, :100] = False; m[:, 400:] = False
    ys, xs = np.nonzero(m)
    if len(xs) > 60:
        out.append('%s %.1f,%.1f' % (os.path.basename(f)[:3], xs.mean(), ys.mean()))
print(' | '.join(out[:int(sys.argv[2]) if len(sys.argv) > 2 else 40]))
