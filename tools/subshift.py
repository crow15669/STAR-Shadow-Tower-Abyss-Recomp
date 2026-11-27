"""subshift.py <dir> <first> <last> [x0 y0 x1 y1]: horizontal and vertical movement of a patch between consecutive
snapshots, to a fraction of a pixel (the peak of the patch's cross-correlation, refined on a parabola)."""
import sys, os, numpy as np
argv = sys.argv
sys.argv = ['x', 'none']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shots.py')).read().split("cmd = sys.argv[1]")[0])
D, first, last = argv[1], int(argv[2]), int(argv[3])
x0, y0, x1, y1 = (int(v) for v in argv[4:8]) if len(argv) > 7 else (200, 120, 440, 320)
def patch(n):
    a = load(os.path.join(D, f"field-{n}.bmp")).astype(np.float64).mean(axis=2)
    return a
def measure(a, b, m=24):
    p = a[y0:y1, x0:x1]
    p = p - p.mean()
    best = None
    score = np.zeros((2 * 3 + 1, 2 * m + 1))
    for dy in range(-3, 4):
        for dx in range(-m, m + 1):
            q = b[y0 + dy:y1 + dy, x0 + dx:x1 + dx]
            q = q - q.mean()
            score[dy + 3, dx + m] = (p * q).sum() / (np.sqrt((p * p).sum() * (q * q).sum()) + 1e-9)
    iy, ix = np.unravel_index(score.argmax(), score.shape)
    def refine(s, i):
        if i <= 0 or i >= len(s) - 1: return 0.0
        d = s[i - 1] - 2 * s[i] + s[i + 1]
        return 0.0 if d == 0 else 0.5 * (s[i - 1] - s[i + 1]) / d
    return ix - m + refine(score[iy], ix), iy - 3 + refine(score[:, ix], iy), score.max()
prev = patch(first)
out = []
for n in range(first + 1, last + 1):
    cur = patch(n)
    dx, dy, s = measure(prev, cur)
    out.append((n, dx, dy, s))
    prev = cur
print(' '.join(f"{n}:{dx:+.2f}/{dy:+.2f}" for n, dx, dy, s in out))
xs = np.array([dx for _, dx, _, _ in out])
if len(xs) > 4:
    odd, even = xs[0::2], xs[1::2]
    print(f"horizontal step: every other field {odd.mean():+.3f} (spread {odd.std():.3f}), the fields between {even.mean():+.3f} (spread {even.std():.3f})")
