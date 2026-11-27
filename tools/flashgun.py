"""flashgun.py <F1 folder>: in the pictures with a muzzle flash (many very bright pixels), where the flash's middle is
and where the gun's top is (the highest light-grey pixels in the lower middle), and the flash's place counted from the
gun's: the same in all of a shot's pictures when the flash sits on the barrel."""
import sys, os, glob
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngread
rows = []
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.png'))):
    a = pngread.load(f).astype(int)
    b = a.max(axis=-1) > 215
    b[:60, :] = False
    b[:, 380:] = False   # the figure at the right
    ys, xs = np.nonzero(b)
    rows.append((os.path.basename(f), int(b.sum()), a, xs, ys))
base = np.median([r[1] for r in rows])
for name, n, a, xs, ys in rows:
    if n < base + 150:
        continue
    fx, fy = xs.mean(), ys.mean()
    # the gun: grey-blue pixels (not the orange of the flash) below the flash's middle
    r, g, bl = a[..., 0], a[..., 1], a[..., 2]
    gun = (np.abs(r - bl) < 25) & (r > 45) & (r < 200)
    gun[:int(fy), :] = False
    gun[:, :int(fx) - 40] = False
    gun[:, int(fx) + 60:] = False
    gy, gx = np.nonzero(gun)
    if len(gx) == 0:
        print(name, 'flash %.1f %.1f, no gun found' % (fx, fy)); continue
    top = gy.min() + 12
    sel = gy <= top
    print('%s flash %6.1f %6.1f  gun top %6.1f %6.1f  flash from gun %+6.1f %+6.1f' % (name, fx, fy, gx[sel].mean(), gy[sel].mean(), fx - gx[sel].mean(), fy - gy[sel].mean()))
