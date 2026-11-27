"""24/32-bit BMP -> PNG without extra libraries: python -I tools/bmp2png.py <in.bmp> [...]  (writes <in>.png)"""
import struct, sys, zlib
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10); w, h = struct.unpack_from('<ii', d, 18); bpp, = struct.unpack_from('<H', d, 28)
    flip = h > 0; h = abs(h); bpx = bpp // 8; stride = (w * bpx + 3) & ~3
    raw = bytearray()
    for y in range(h):
        row = d[off + (h - 1 - y if flip else y) * stride:][:w * bpx]
        raw.append(0)
        px = bytearray(w * 3)
        px[0::3] = row[2::bpx]; px[1::3] = row[1::bpx]; px[2::3] = row[0::bpx]
        raw += px
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    open(path[:-4] + '.png', 'wb').write(png)
    print(path, w, h, bpp)
