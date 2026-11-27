"""pacedelay.py <pace log> <present stats csv> [skip seconds]: for a VISIBLE run with both PS2X_PCSX2_PACE_LOG and
PS2X_PRESENT_STATS set: when each queued picture really came on screen, against its present call and the refresh it
was planned for. Kinds: k = the host's in-between pictures (3D with smooth motion; planned for the refresh after the
one they are handed over at), p = ordinary pictures with PS2X_PCSX2_PACE=1 (planned for their `target`).
"On screen at the planned refresh +N" is the row's slip: +0 everywhere is right; +1, +2 = that many refreshes of
delay nobody planned."""
import sys, csv, bisect
pace = list(csv.DictReader(open(sys.argv[1])))
stats = list(csv.DictReader(open(sys.argv[2])))
skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
TICK = 100  # ns per QueryPerformanceCounter tick on this kind of PC (10 MHz)
# the screen's refresh period from the statistics themselves
a, b = stats[len(stats) // 4], stats[-5]
period = (int(b['sync_qpc']) - int(a['sync_qpc'])) * TICK / max(1, int(b['sync_refresh']) - int(a['sync_refresh']))
first_row = {}  # present number -> time its row was written (right after the present call returned)
shown = {}      # present number -> time (ns) of the refresh it was first seen on
for r in stats:
    first_row.setdefault(int(r['last_present']), int(r['qpc']) * TICK)
    s = int(r['shown_present'])
    if int(r['shown_refresh']) and s not in shown:
        shown[s] = int(r['sync_qpc']) * TICK + (int(r['shown_refresh']) - int(r['sync_refresh'])) * period
counts = sorted(first_row)
times = [first_row[c] for c in counts]
queued = {r['time']: r for r in pace if r['kind'] == 'q'}
t0 = int(pace[0]['now'])
print(f'screen {1e9 / period:.2f} Hz (one refresh = {period / 1e6:.2f} ms)')
def med(v):
    v = sorted(v)
    return v[len(v) // 2]
for kind, name in (('k', "in-between pictures (3D)"), ('p', 'ordinary pictures, paced')):
    rows = []
    for pr in pace:
        if pr['kind'] != kind:
            continue
        now = int(pr['now'])
        if (now - t0) / 1e9 < skip:
            continue
        if kind == 'p':
            if pr['time'] not in queued:
                continue
            planned = int(queued[pr['time']]['target'])
        else:
            planned = int(pr['time']) - 1000000 + period
        i = bisect.bisect_left(times, now)
        if i >= len(times) or times[i] - now > 40e6:
            continue
        c = counts[i]
        if c not in shown:
            continue
        rows.append((shown[c] - now, (shown[c] - planned) / period, times[i] - now, now - int(pr['time']), (now - t0) / 1e9))
    if len(rows) < 10:
        continue
    n = len(rows)
    print(f'{name}: {n}')
    print(f'  present call -> on screen: median {med([r[0] for r in rows]) / 1e6:.1f} ms, least {min(r[0] for r in rows) / 1e6:.1f}, most {max(r[0] for r in rows) / 1e6:.1f} ms')
    print(f'  the present call itself took: median {med([r[2] for r in rows]) / 1e6:.2f} ms, 90% under {sorted(r[2] for r in rows)[int(n * 0.9)] / 1e6:.2f} ms')
    print(f'  called after its hand-over time by: median {med([r[3] for r in rows]) / 1e6:.2f} ms, 99% under {sorted(r[3] for r in rows)[int(n * 0.99)] / 1e6:.2f} ms')
    off = {}
    for r in rows:
        k = round(r[1])
        off[k] = off.get(k, 0) + 1
    print('  on screen at the planned refresh +N: ' + ', '.join(f'{k:+d}: {v}' for k, v in sorted(off.items())))
    # the slip over time, one figure per second
    per = {}
    for r in rows:
        per.setdefault(int(r[4]), []).append(r[1])
    print('  slip by second: ' + ' '.join(f'{sum(v) / len(v):.1f}' for _, v in sorted(per.items())))
