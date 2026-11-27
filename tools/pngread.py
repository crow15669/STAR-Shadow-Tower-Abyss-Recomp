"""pngread.py: load(path) -> numpy array (h, w, 3) uint8 for 8-bit RGB / RGBA non-interlaced PNG files (the renderer's dumps)."""
import struct, zlib
import numpy as np

def load(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    p = 8; idat = b''; w = h = ct = 0
    while p < len(d):
        n, t = struct.unpack_from('>I4s', d, p)
        body = d[p + 8:p + 8 + n]
        if t == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
            assert bd == 8 and il == 0 and ct in (2, 6)
        elif t == b'IDAT':
            idat += body
        p += 12 + n
    bpp = 3 if ct == 2 else 4
    raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, 1 + w * bpp)
    out = np.zeros((h, w * bpp), np.uint8)
    prev = np.zeros(w * bpp, np.int32)
    for y in range(h):
        f = raw[y, 0]; line = raw[y, 1:].astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 255
        elif f == 1:
            cur = line.reshape(w, bpp).cumsum(axis=0).reshape(-1) & 255
        else:
            cur = np.zeros(w * bpp, np.int32)
            for i in range(w * bpp):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
                if f == 3:
                    cur[i] = (line[i] + ((a + b) >> 1)) & 255
                else:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    cur[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y] = cur; prev = cur
    return out.reshape(h, w, bpp)[:, :, :3]
