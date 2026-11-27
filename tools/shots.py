"""Snapshot helpers (numpy only).
  shots.py bright <in.bmp> <out.png> [gain] [x0 y0 x1 y1] [zoom]
  shots.py diff <a.bmp> <b.bmp> [threshold]            -> count + bounding box
  shots.py pairs <dirA> <dirB> [threshold]             -> per common file: differing pixels
  shots.py flicker <dir> <step> [threshold]            -> per file n: pixels differing from file n+step
  shots.py diffpng <a.bmp> <b.bmp> <out.png> [gain]    -> a (brightened) with differing pixels in red
"""
import struct, sys, zlib, os, re
import numpy as np


def load(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h = struct.unpack_from('<ii', d, 18)
    bpp, = struct.unpack_from('<H', d, 28)
    flip = h > 0
    h = abs(h)
    bpx = bpp // 8
    stride = (w * bpx + 3) & ~3
    a = np.frombuffer(d, np.uint8, stride * h, off).reshape(h, stride)[:, :w * bpx].reshape(h, w, bpx)[:, :, 2::-1]
    return a[::-1] if flip else a


def save(path, a):
    a = np.ascontiguousarray(a.astype(np.uint8))
    h, w, _ = a.shape
    raw = np.concatenate([np.zeros((h, 1), np.uint8), a.reshape(h, w * 3)], axis=1).tobytes()
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def bright(a, gain):
    return np.clip(255.0 * (a / 255.0) ** (1.0 / gain), 0, 255)


def mask(a, b, t):
    return (np.abs(a.astype(int) - b.astype(int)).max(axis=2) > t)


def bbox(m):
    ys, xs = np.nonzero(m)
    return (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())) if len(xs) else None


def num(name):
    return int(re.findall(r'\d+', name)[-1])


cmd = sys.argv[1]
if cmd == 'bright':
    a = load(sys.argv[2])
    gain = float(sys.argv[4]) if len(sys.argv) > 4 else 2.5
    a = bright(a, gain)
    if len(sys.argv) > 8:
        x0, y0, x1, y1 = map(int, sys.argv[5:9])
        a = a[y0:y1, x0:x1]
    zoom = int(sys.argv[9]) if len(sys.argv) > 9 else 1
    if zoom > 1:
        a = a.repeat(zoom, axis=0).repeat(zoom, axis=1)
    save(sys.argv[3], a)
elif cmd == 'diff':
    t = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    m = mask(load(sys.argv[2]), load(sys.argv[3]), t)
    print(int(m.sum()), bbox(m))
elif cmd == 'pairs':
    t = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    A, B = sys.argv[2], sys.argv[3]
    names = sorted(set(f for f in os.listdir(A) if f.endswith('.bmp')) & set(os.listdir(B)), key=num)
    out = []
    for f in names:
        m = mask(load(os.path.join(A, f)), load(os.path.join(B, f)), t)
        out.append(f"{num(f)}:{int(m.sum())}")
    print(' '.join(out))
elif cmd == 'flicker':
    step = int(sys.argv[3])
    t = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    D = sys.argv[2]
    names = sorted((f for f in os.listdir(D) if f.endswith('.bmp')), key=num)
    out = []
    for i in range(len(names) - step):
        m = mask(load(os.path.join(D, names[i])), load(os.path.join(D, names[i + step])), t)
        out.append(f"{num(names[i])}:{int(m.sum())}")
    print(' '.join(out))
elif cmd == 'diffpng':
    a, b = load(sys.argv[2]), load(sys.argv[3])
    gain = float(sys.argv[5]) if len(sys.argv) > 5 else 2.5
    m = mask(a, b, 8)
    o = bright(a, gain)
    o[m] = (255, 0, 0)
    save(sys.argv[4], o)
    print(int(m.sum()), bbox(m))
