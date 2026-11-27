"""fsheet.py <F1 folder> <out.png> <first> <last> <step> [cols] [scale] [gain]: contact sheet of an F1 catch's
pictures (NNN-k<id>.png), every <step>-th by the leading number from <first> to <last>, shrunk by <scale>, each
labelled with its number in the top left corner (as dots: none; the order is left to right, top to bottom)."""
import sys, os
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import pngread
argv = sys.argv
sys.argv = ['x', 'none']
exec(open(os.path.join(S, 'shots.py')).read().split("cmd = sys.argv[1]")[0])
D, out, first, last, step = argv[1], argv[2], int(argv[3]), int(argv[4]), int(argv[5])
cols = int(argv[6]) if len(argv) > 6 else 4
scale = int(argv[7]) if len(argv) > 7 else 1
gain = float(argv[8]) if len(argv) > 8 else 1.0
files = sorted(f for f in os.listdir(D) if f.endswith('.png') and f[:3].isdigit())
pick = [f for f in files if first <= int(f[:3]) <= last and (int(f[:3]) - first) % step == 0]
tiles = []
for f in pick:
    a = np.asarray(pngread.load(os.path.join(D, f)))[..., :3].astype(np.float32)
    a = np.clip(a * gain, 0, 255).astype(np.uint8)
    if scale > 1:
        h, w = a.shape[0] // scale * scale, a.shape[1] // scale * scale
        a = a[:h, :w].reshape(h // scale, scale, w // scale, scale, 3).mean(axis=(1, 3)).astype(np.uint8)
    a = np.pad(a, ((0, 4), (0, 4), (0, 0)), constant_values=255)
    tiles.append(a)
while len(tiles) % cols:
    tiles.append(np.full_like(tiles[0], 255))
rows = [np.concatenate(tiles[i:i + cols], axis=1) for i in range(0, len(tiles), cols)]
save(out, np.concatenate(rows, axis=0))
print(len(pick), 'pictures:', ' '.join(f[:3] for f in pick))
