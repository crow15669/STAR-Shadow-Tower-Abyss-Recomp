"""bmpsmall.py <in.bmp> <out.png> [width]: a snapshot (24- or 32-bit BMP) as a PNG, shrunk to about `width` (default 640)."""
import sys, os, struct
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngcrop
d = open(sys.argv[1], 'rb').read()
off, = struct.unpack_from('<I', d, 10)
w, h, planes, bpp = struct.unpack_from('<iiHH', d, 18)
n = bpp // 8
row = (w * n + 3) & ~3
a = np.frombuffer(d, dtype=np.uint8, count=row * abs(h), offset=off).reshape(abs(h), row)[:, :w * n].reshape(abs(h), w, n)
if h > 0:
    a = a[::-1]
a = a[:, :, 2::-1]
step = max(1, w // (int(sys.argv[3]) if len(sys.argv) > 3 else 640))
pngcrop.save(sys.argv[2], a[::step, ::step])
