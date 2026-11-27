"""psheetb.py <dir> <out.png> <first> <last> <step> <cols> <scale> <gain>: psheet.py with the pictures brightened."""
import sys, os, re, zlib, struct
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import pngread
D, out, first, last, step, cols, scale = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:8])
gain = float(sys.argv[8])
files = {}
for f in os.listdir(D):
    m = re.match(r'present-(\d+)-', f)
    if m:
        files[int(m.group(1))] = f
tiles = [np.clip(np.asarray(pngread.load(os.path.join(D, files[n])))[::scale, ::scale, :3].astype(np.float32) * gain, 0, 255).astype(np.uint8)
         for n in range(first, last + 1, step) if n in files]
h, w = tiles[0].shape[:2]
rows = (len(tiles) + cols - 1) // cols
sheet = np.zeros((rows * (h + 4), cols * (w + 4), 3), dtype=np.uint8)
for i, t in enumerate(tiles):
    r, c = divmod(i, cols)
    sheet[r * (h + 4): r * (h + 4) + h, c * (w + 4): c * (w + 4) + w] = t
def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
raw = b''.join(b'\x00' + sheet[y].tobytes() for y in range(sheet.shape[0]))
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', sheet.shape[1], sheet.shape[0], 8, 2, 0, 0, 0)) +
                      chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
print(len(tiles), 'tiles')
