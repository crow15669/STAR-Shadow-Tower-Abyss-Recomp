"""selftest.py <log> <dump dir> [worst]: with STAR_SMOOTH_SELFTEST=1 every picture of a game frame must be the same
(the game's own picture first, then passes placed at the frame's own state). Compares the presented pictures
(PS2X_PCSX2_PRESENT_DUMP files) inside each frame's group of ids from the log's [selftest] lines."""
import sys, os, re
import numpy as np
S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, S)
import pngread
log, D = sys.argv[1], sys.argv[2]
worst = int(sys.argv[3]) if len(sys.argv) > 3 else 8
files = {}
for f in os.listdir(D):
    m = re.match(r'present-(\d+)-id(\d+)\.png', f)
    if m:
        files[int(m.group(2))] = f
groups = []
for line in open(log, encoding='utf-8', errors='replace'):
    if line.startswith('[selftest]'):
        # the ids up to the first thing that is not a number (another thread's log line can land on the same line)
        ids = []
        for x in line.split()[1:]:
            if not x.isdigit():
                break
            ids.append(int(x))
        if len(ids) > 1 and all(i in files for i in ids):
            groups.append(ids)
results = []
for ids in groups:
    own = np.asarray(pngread.load(os.path.join(D, files[ids[0]])))[..., :3].astype(np.int16)
    for i in ids[1:]:
        other = np.asarray(pngread.load(os.path.join(D, files[i])))[..., :3].astype(np.int16)
        d = np.abs(own - other).max(axis=2)
        results.append((float((d > 24).mean()) * 100.0, float(d.mean()), files[ids[0]], files[i]))
if not results:
    print('no complete groups: check the dump range and the log')
    sys.exit(0)
bad = [r for r in results if r[0] > 0.2]
print(f'{len(groups)} frames, {len(results)} pass pictures compared with their frame\'s own picture')
print(f'pictures with more than 0.2% of pixels clearly different: {len(bad)}')
print(f'median different pixels {np.median([r[0] for r in results]):.3f}%, largest {max(r[0] for r in results):.2f}%')
for r in sorted(results, reverse=True)[:worst]:
    print(f'  {r[0]:6.2f}% different, mean {r[1]:5.2f}: {r[2]} against {r[3]}')
