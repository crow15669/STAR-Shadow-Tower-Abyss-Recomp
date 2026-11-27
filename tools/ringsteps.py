"""ringsteps.py <F1 folder> [first last]: for every recorded picture (sorted by number) the mean difference from the
picture before and the sideways / up-down shift (pixels at 480x360, by matching a middle band and a middle column
strip), next to its id and time. A flicker = big differences or shifts going back and forth.
ringsteps.py <F1 folder> sheet <out.png> <first> <last> [cols]: contact sheet of those pictures."""
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
        files[int(m.group(1))] = (f, m.group(2), int(m.group(3)))
times = {}
for line in open(os.path.join(D, 'list.txt')):
    m = re.match(r'(\d+)-\S+\s+([\d.]+) ms', line)
    if m:
        times[int(m.group(1))] = float(m.group(2))


def load(n):
    return np.asarray(pngread.load(os.path.join(D, files[n][0])))[..., :3].astype(np.float32)


if len(sys.argv) > 2 and sys.argv[2] == 'sheet':
    out, first, last = sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
    cols = int(sys.argv[6]) if len(sys.argv) > 6 else 6
    tiles = [load(n).astype(np.uint8) for n in range(first, last + 1) if n in files]
    h, w = tiles[0].shape[:2]
    rows = (len(tiles) + cols - 1) // cols
    sheet = np.zeros((rows * (h + 4), cols * (w + 4), 3), dtype=np.uint8)
    for i, t in enumerate(tiles):
        r, c = divmod(i, cols)
        sheet[r * (h + 4): r * (h + 4) + h, c * (w + 4): c * (w + 4) + w] = t
    import zlib, struct

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    raw = b''.join(b'\x00' + sheet[y].tobytes() for y in range(sheet.shape[0]))
    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', sheet.shape[1], sheet.shape[0], 8, 2, 0, 0, 0)) +
                          chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    print('sheet', len(tiles))
    sys.exit(0)


def shift(a, b, axis, most=60):
    """how far b is a shifted along the axis (grey bands), by least mean difference"""
    best, where = 1e18, 0
    for s in range(-most, most + 1):
        if axis == 1:
            x = a[:, most + s: a.shape[1] - most + s]
            y = b[:, most: b.shape[1] - most]
        else:
            x = a[most + s: a.shape[0] - most + s]
            y = b[most: b.shape[0] - most]
        d = float(np.abs(x - y).mean())
        if d < best:
            best, where = d, s
    return where, best


first = int(sys.argv[2]) if len(sys.argv) > 3 else min(files)
last = int(sys.argv[3]) if len(sys.argv) > 3 else max(files)
before = None
for n in range(first, last + 1):
    if n not in files:
        continue
    p = load(n).mean(axis=2)
    if before is not None:
        mean = float(np.abs(p - before).mean())
        sx, rx = shift(before[100:260], p[100:260], 1)
        sy, ry = shift(before[:, 140:340], p[:, 140:340], 0)
        print(f"{n:3d} {files[n][1]}{files[n][2]} {times.get(n, 0):7.1f} ms  diff {mean:6.2f}  x {sx:+4d} ({rx:5.2f})  y {sy:+4d} ({ry:5.2f})")
    before = p
