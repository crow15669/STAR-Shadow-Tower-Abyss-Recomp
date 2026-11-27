"""presentstats.py <csv> [seconds per row]: how many screen refreshes each presented picture stayed on screen, from a
PS2X_PRESENT_STATS file (visible runs only), in windows of time. An even run shows one number only (e.g. all 2)."""
import sys, csv
rows = list(csv.DictReader(open(sys.argv[1])))
window = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
first = {}  # presented picture -> (refresh it was first seen on screen, qpc)
for r in rows:
    p, refresh, qpc = int(r['shown_present']), int(r['shown_refresh']), int(r['sync_qpc'])
    if refresh and p not in first:
        first[p] = (refresh, qpc)
ps = sorted(first)
if len(ps) < 3:
    print('too few pictures')
    sys.exit(0)
t0 = first[ps[0]][1]
qpcs = [first[p][1] for p in ps]
freq = 1e7  # QueryPerformanceFrequency on this kind of PC
out = {}
skipped = {}
for a, b in zip(ps, ps[1:]):
    w = int((first[a][1] - t0) / freq / window)
    held = first[b][0] - first[a][0]
    out.setdefault(w, {}).setdefault(held, 0)
    out[w][held] += 1
    skipped[w] = skipped.get(w, 0) + (b - a - 1)
# The hold each present asked for (the column `hold`, newer files): does a picture stay as long as its own present
# said, or as long as the next one's said? (DXGI's sync interval; GSRenderer::PaceOrdinary counts on the second.)
hold = {}
for r in rows:
    if r.get('hold') not in (None, ''):
        hold.setdefault(int(r['last_present']), int(r['hold']))
if hold:
    own = nxt = n = 0
    for a, b in zip(ps, ps[1:]):
        if b != a + 1 or a not in hold or b not in hold or hold[a] <= 0 or hold[b] <= 0:
            continue
        held = first[b][0] - first[a][0]
        n += 1
        own += held == hold[a]
        nxt += held == hold[b]
    if n:
        print(f'{n} pictures with a hold asked: on screen as long as their own present said {100 * own / n:.1f}%, '
              f'as long as the next one said {100 * nxt / n:.1f}%')
print('time (s)  pictures/s  on screen for N refreshes: count                      never shown')
for w in sorted(out):
    n = sum(out[w].values())
    parts = ', '.join(f'{k}: {v}' for k, v in sorted(out[w].items()))
    print(f'{w * window:6.0f}    {n / window:8.1f}    {parts:50s} {skipped[w]}')
