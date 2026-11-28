"""pacewhy.py <pace log> [n] [min id]: for every in-between picture: when it was ready (a / r), when its present was
called (k) and returned (e), against its time. Says how many were late because they were not ready and how many
because the GS thread came late, and for the first n late ones lists what that thread did in the 30 ms before. Times
in ms relative to the picture's time. The log: PS2X_PCSX2_PACE_LOG=<file> (works in hidden runs)."""
import sys, csv
rows = list(csv.DictReader(open(sys.argv[1])))
show = int(sys.argv[2]) if len(sys.argv) > 2 else 12
minid = int(sys.argv[3]) if len(sys.argv) > 3 else 0
ev = []
for r in rows:
    ev.append((int(r['now']), r['kind'], int(r['target']), int(r['time'])))
ready, called, done = {}, {}, {}
order = []
for now, kind, pid, t in ev:
    if kind in 'ar':
        ready[pid] = (now, kind, t)
    elif kind == 'k':
        called[pid] = now
        order.append(pid)
    elif kind == 'e':
        done[pid] = now
ms = lambda v: v / 1e6
late = []
n_ready_late = n_poll_late = 0
calls = []
for pid in order:
    if pid not in ready or pid < minid:
        continue
    rnow, rk, t = ready[pid]
    c = called[pid]
    d = done.get(pid, c)
    calls.append(ms(d - c))
    if ms(c - t) > 2.0:
        late.append((pid, rk, ms(rnow - t), ms(c - t), ms(d - c)))
        if ms(rnow - t) > 0.5:
            n_ready_late += 1
        else:
            n_poll_late += 1
calls.sort()
print(f"{len(order)} in-between pictures; present calls: median {calls[len(calls) // 2]:.2f} ms, 95% {calls[int(len(calls) * 0.95)]:.2f}, 99% {calls[int(len(calls) * 0.99)]:.2f}, max {calls[-1]:.2f}")
print(f"{len(late)} called over 2 ms late: {n_ready_late} were not ready by their time, {n_poll_late} were ready but the thread came late")
vs = [(now, t) for now, kind, pid, t in ev if kind == 'V']
vd = sorted(ms(t - now) for now, t in vs)
if vd:
    print(f"{len(vd)} vsyncs handled: median {vd[len(vd) // 2]:.2f} ms, 95% {vd[int(len(vd) * 0.95)]:.2f}, max {vd[-1]:.2f}")
print("ready (kind) at / present called at / call took, ms against the picture's time:")
shown = 0
for pid, rk, r, c, d in late:
    if shown >= show:
        break
    shown += 1
    t = ready[pid][2]
    print(f"  id {pid} ({rk}): ready {r:+6.2f}, called {c:+6.2f}, took {d:.2f}")
    for now, kind, p2, t2 in ev:
        if t - 30e6 <= now <= called[pid] + 1e6:
            extra = f" until {ms(t2 - t):+7.2f}" if kind == 'V' else (f" id {p2} due {ms(t2 - t):+7.2f}" if kind in 'arke' else '')
            print(f"        {ms(now - t):+7.2f}  {kind}{extra}")
