"""latelooks.py <log with STAR_LIVE_LOG=1>: from the [late] lines, each picture's direction (degrees) and its step
from the picture before; counts the steps that go against the turn (the sign of the three steps around them)."""
import sys, re
rows = []
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r"\[late\] frame (\d+) pass (\d+) t ([\d.]+):.*looks ([-\d.]+); from the test's camera wanted ([-+\d.]+) given ([-+\d.]+)", line)
    if m:
        rows.append((int(m.group(1)), int(m.group(2)), float(m.group(4)), float(m.group(5)), float(m.group(6))))
steps = []
for a, b in zip(rows, rows[1:]):
    d = b[2] - a[2]
    d = (d + 180.0) % 360.0 - 180.0
    steps.append(d)
against = 0
out = []
for i, d in enumerate(steps):
    around = steps[max(0, i - 2): i] + steps[i + 1: i + 3]
    way = sum(1 if x > 0.05 else -1 if x < -0.05 else 0 for x in around)
    bad = (way >= 3 and d < -0.05) or (way <= -3 and d > 0.05)
    against += bad
    out.append(('%+.1f' % d) + ('!' if bad else '') + ('|' if rows[i + 1][1] == 0 else ''))
first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
last = int(sys.argv[3]) if len(sys.argv) > 3 else len(out)
print(' '.join(out[first:last]))
print('pictures', len(rows), 'steps against the turn', against)
