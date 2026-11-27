"""blink.py <dir> [<dir> ...] [--t N] [--png out.png]: counts "blink" pixels (value jumps for one field and comes back)
over a run of consecutive snapshots; with --png writes a heat map over the middle frame of the first folder."""
import sys, os, re
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys_argv = sys.argv
sys.argv = ['x', 'none']
exec(open(os.path.join(S, 'shots.py')).read().split("cmd = sys.argv[1]")[0])
args = sys_argv[1:]
T = 12
png = None
dirs = []
i = 0
while i < len(args):
    if args[i] == '--t':
        T = int(args[i + 1]); i += 2
    elif args[i] == '--png':
        png = args[i + 1]; i += 2
    else:
        dirs.append(args[i]); i += 1
for k, D in enumerate(dirs):
    names = sorted((f for f in os.listdir(D) if f.endswith('.bmp')), key=num)
    frames = np.stack([load(os.path.join(D, f)).astype(np.int16) for f in names])
    L = frames.max(axis=3)
    a, b, c = L[:-2], L[1:-1], L[2:]
    blink = (np.abs(b - a) > T) & (np.abs(b - c) > T) & (np.abs(a - c) < T // 2)
    per = blink.sum(axis=(1, 2))
    heat = blink.sum(axis=0)
    print(f"{D}: frames {len(names)}, blink pixels total {int(per.sum())}, per field median {int(np.median(per))} max {int(per.max())}"
          f" (field {num(names[int(per.argmax())+1])}); pixels blinking 3+ times {int((heat >= 3).sum())}")
    if png and k == 0:
        mid = frames[len(names) // 2].astype(float)
        o = bright(mid, 3.5)
        o[heat >= 2] = (255, 0, 0)
        o[heat == 1] = (255, 160, 0)
        save(png, o)
