"""turnsteps.py <folder of PS2X_PCSX2_PRESENT_DUMP pictures> [montage.png]: how far the picture moved sideways
between consecutive presented pictures (pixels), for a test in which the view turns at a steady pace
(STAR_LIVE_TEST). An even row of steps means the turn is shown at an even pace; the mean of the steps tells the
turning speed on screen. Writes a strip of the first pictures' right-hand thirds when a second path is given."""
import os, re, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngread

folder = sys.argv[1]
names = sorted((n for n in os.listdir(folder) if n.endswith('.png')), key=lambda n: int(re.findall(r'\d+', n)[0]))
pictures = []
for n in names:
    a = pngread.load(os.path.join(folder, n)).astype(np.float32)
    pictures.append(a[..., :3].mean(axis=2))


def shift(a, b, most=220):
    """b is a moved sideways by how many pixels (best match of the middle rows' columns)"""
    h, w = a.shape
    band_a = a[h // 4: 3 * h // 4]
    band_b = b[h // 4: 3 * h // 4]
    best, best_error = 0, 1e30
    for d in range(-most, most + 1):
        if d >= 0:
            x, y = band_a[:, d:], band_b[:, :w - d]
        else:
            x, y = band_a[:, :w + d], band_b[:, -d:]
        cut = slice(w // 8, x.shape[1] - w // 8)
        error = float(np.mean((x[:, cut] - y[:, cut]) ** 2))
        if error < best_error:
            best, best_error = d, error
    return best


steps = [shift(pictures[i], pictures[i + 1]) for i in range(len(pictures) - 1)]
ids = [int(re.findall(r'\d+', n)[1]) if len(re.findall(r'\d+', n)) > 1 else 0 for n in names]
print('pictures', len(pictures), 'size', pictures[0].shape[::-1])
print('steps (pixels):', ' '.join(str(s) for s in steps))
arr = np.array(steps, dtype=np.float64)
print('mean %.2f, spread (standard deviation) %.2f, smallest %d, largest %d' % (arr.mean(), arr.std(), arr.min(), arr.max()))
