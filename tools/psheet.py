"""psheet.py <dir> <out.png> <first> <last> <step> [cols] [scale]: contact sheet of presented pictures
(PS2X_PCSX2_PRESENT_DUMP files, present-<n>-id<id>.png), every <step>-th from <first> to <last>, shrunk by <scale>."""
import sys, os, re
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import pngread
argv = sys.argv
sys.argv = ['x', 'none']
exec(open(os.path.join(S, 'shots.py')).read().split("cmd = sys.argv[1]")[0])
D, out, first, last, step = argv[1], argv[2], int(argv[3]), int(argv[4]), int(argv[5])
cols = int(argv[6]) if len(argv) > 6 else 4
scale = int(argv[7]) if len(argv) > 7 else 2
files = {}
for f in os.listdir(D):
    m = re.match(r'present-(\d+)-', f)
    if m:
        files[int(m.group(1))] = f
tiles = []
for n in range(first, last + 1, step):
    if n in files:
        a = np.asarray(pngread.load(os.path.join(D, files[n])))[..., :3]
        tiles.append(a[::scale, ::scale])
h, w = tiles[0].shape[:2]
rows = (len(tiles) + cols - 1) // cols
sheet = np.zeros((rows * (h + 4), cols * (w + 4), 3), dtype=np.uint8)
for i, t in enumerate(tiles):
    r, c = divmod(i, cols)
    sheet[r * (h + 4): r * (h + 4) + h, c * (w + 4): c * (w + 4) + w] = t
save(out, sheet)
print(len(tiles), 'tiles', sheet.shape)
