"""pngcrop.py <out.png> <scale> <x0> <y0> <x1> <y1> <picture> [picture ...]: the same cut of several pictures, side by
side and enlarged (to compare small things between pictures; no PIL needed)."""
import sys, os, zlib, struct
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngread

def save(path, a):
    h, w, _ = a.shape
    raw = b''.join(b'\x00' + a[y].astype(np.uint8).tobytes() for y in range(h))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

if __name__ == '__main__':
    out, scale = sys.argv[1], int(sys.argv[2])
    x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
    cuts = []
    for f in sys.argv[7:]:
        a = pngread.load(f)[y0:y1, x0:x1, :3]
        a = np.repeat(np.repeat(a, scale, axis=0), scale, axis=1)
        cuts.append(a)
        cuts.append(np.full((a.shape[0], 4, 3), 255, dtype=a.dtype))
    save(out, np.concatenate(cuts[:-1], axis=1))
