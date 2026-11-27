"""sheet.py <dir> <out.png> x0 y0 x1 y1 zoom cols [gain] [first last step]: contact sheet of a crop over the fields."""
import sys, os
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))  # shots.py is next to this file
argv = sys.argv
sys.argv = ['x', 'none']
exec(open(os.path.join(S, 'shots.py')).read().split("cmd = sys.argv[1]")[0])
D, out = argv[1], argv[2]
x0, y0, x1, y1, zoom, cols = map(int, argv[3:9])
gain = float(argv[9]) if len(argv) > 9 else 1.6
names = sorted((f for f in os.listdir(D) if f.endswith('.bmp')), key=num)
if len(argv) > 12:
    first, last, step = map(int, argv[10:13])
    names = [f for f in names if first <= num(f) <= last and (num(f) - first) % step == 0]
tiles = []
for f in names:
    a = bright(load(os.path.join(D, f)), gain)[y0:y1, x0:x1].repeat(zoom, axis=0).repeat(zoom, axis=1)
    a = np.pad(a, ((0, 4), (0, 4), (0, 0)), constant_values=255)
    tiles.append(a)
while len(tiles) % cols:
    tiles.append(np.zeros_like(tiles[0]))
rows = [np.concatenate(tiles[i:i + cols], axis=1) for i in range(0, len(tiles), cols)]
save(out, np.concatenate(rows, axis=0))
print(' '.join(str(num(f)) for f in names))
