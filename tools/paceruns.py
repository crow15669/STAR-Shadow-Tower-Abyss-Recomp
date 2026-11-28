"""paceruns.py <pace log> [min id] [n]: the GS thread's time for each run over a frame, from one marker to the next
(5 = the own run begins, 1 = a picture kept, 2 = repeated, 3 = passes begin, 4 = passes over, 6 = a frame without
passes over). Prints the spread of marker-to-marker times by kind, and the timeline around the n longest."""
import sys, csv
rows = list(csv.DictReader(open(sys.argv[1])))
minid = int(sys.argv[2]) if len(sys.argv) > 2 else 0
show = int(sys.argv[3]) if len(sys.argv) > 3 else 4
ev = [(int(r['now']), r['kind'], int(r['target']), int(r['time']), int(r['hold'])) for r in rows]
# start once an id >= minid has been seen
start = next((i for i, e in enumerate(ev) if e[1] == 'a' and e[2] >= minid), 0)
ev = ev[start:]
ms = lambda v: v / 1e6
spans = {}   # (from kind, to kind) -> list of (ms, index)
last = None
for i, (now, kind, pid, t, hold) in enumerate(ev):
    if kind != 'm':
        continue
    if last is not None:
        spans.setdefault((last[1], hold), []).append((ms(now - last[0]), i))
    last = (now, hold)
names = {(5, 3): "own run of a frame with passes (5 -> 3)", (3, 1): "first pass (3 -> 1)", (1, 1): "a later pass (1 -> 1)",
         (1, 4): "after the last pass (1 -> 4)", (4, 5): "idle until the next frame (4 -> 5)", (5, 1): "own run of a frame without passes (5 -> 1)",
         (6, 5): "idle until the next frame (6 -> 5)", (1, 2): "(1 -> 2)", (2, 2): "(2 -> 2)", (2, 6): "(2 -> 6)", (1, 6): "(1 -> 6)"}
for key in sorted(spans):
    v = sorted(x for x, _ in spans[key])
    print(f"  {names.get(key, str(key)):48s} n {len(v):5d}  median {v[len(v) // 2]:6.2f}  95% {v[int(len(v) * 0.95)]:6.2f}  99% {v[int(len(v) * 0.99)]:6.2f}  max {v[-1]:7.2f}")
work = [(x, i, key) for key in spans if key not in ((4, 5), (6, 5)) for x, i in spans[key]]
print(f"runs over 8 ms: {sum(1 for x, _, _ in work if x > 8)} of {len(work)}")
for x, i, key in sorted(work, reverse=True)[:show]:
    print(f"--- {x:.2f} ms, {names.get(key, str(key))}")
    t0 = ev[i][0]
    for now, kind, pid, t, hold in ev[max(0, i - 22):i + 6]:
        if kind == 'V':
            print(f"   {ms(now - t0):+8.2f}  V  took {ms(t - now):.2f}")
        elif kind == 'm':
            print(f"   {ms(now - t0):+8.2f}  m  kind {hold}" + (f" id {pid}" if hold in (1, 2) else ''))
        else:
            print(f"   {ms(now - t0):+8.2f}  {kind}  id {pid}  due {ms(t - t0):+8.2f}")
