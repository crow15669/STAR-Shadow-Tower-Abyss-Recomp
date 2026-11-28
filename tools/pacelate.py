"""pacelate.py <pace log> [<F1 folder with frames.txt and list.txt>]: how late each in-between picture (k) was
presented against its time, in ms; a histogram, and (with the folder) the lateness by the picture's place in its frame.
The pace log has no ids: the folder's list.txt gives the last pictures' ids in order, matched from the log's end by the
picture written last."""
import sys, os, re, csv
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r['kind'] == 'k']
TICK = 1e-6  # the frame clock is in ns
late = [(int(r['now']) - int(r['time'])) * TICK for r in rows]
print(f"{len(late)} in-between pictures presented")
edges = [-100, 1, 2, 3, 4, 6, 8, 12, 20, 1000]
for a, b in zip(edges, edges[1:]):
    n = sum(1 for v in late if a <= v < b)
    print(f"  {a:5} .. {b:4} ms late: {n:5d}  {100.0 * n / len(late):5.1f}%")
gaps = [(int(b['now']) - int(a['now'])) * TICK for a, b in zip(rows, rows[1:])]
print("gaps between presents over 10.5 ms:", sum(1 for g in gaps if 10.5 < g < 100), " under 3.5 ms:", sum(1 for g in gaps if g < 3.5))
if len(sys.argv) > 2:
    D = sys.argv[2]
    ids = []
    for line in open(os.path.join(D, 'list.txt')):
        m = re.match(r'(\d+)-([kv])(\d+)\.png\s+([\d.]+) ms', line)
        if m:
            ids.append((int(m.group(3)), float(m.group(4))))
    where = {}
    for line in open(os.path.join(D, 'frames.txt'), errors='replace'):
        if line.startswith('F '):
            m = re.match(r'F frame (\d+).*pictures: (.*)', line)
            if m:
                got = re.findall(r'(\d+)@([\d.]+)', m.group(2))
                for k, (i, at) in enumerate(got):
                    where[int(i)] = (int(m.group(1)), k, len(got), float(at))
    # match: the list's newest picture = the log row whose `now` is nearest the clock of the save; simpler: line up by
    # the gaps (the last len(ids) presents before the save are the list's)
    # find the log index whose following gap is the save's stall (over 300 ms)
    end = None
    for i in range(len(gaps) - 1, 0, -1):
        if gaps[i] > 300:
            end = i
    if end is None:
        end = len(rows) - 1
    # the list's times are "ms before the newest": check the line-up
    first = end - len(ids) + 1
    check = (int(rows[end]['now']) - int(rows[first]['now'])) * TICK
    print(f"line-up: the log's span {check:.1f} ms against the list's {ids[0][1]:.1f} ms")
    by = {}
    worst = []
    for j, (i, t) in enumerate(ids):
        w = where.get(i)
        if not w:
            continue
        v = late[first + j]
        by.setdefault(w[1], []).append(v)
        worst.append((v, i, w))
    for k in sorted(by):
        v = sorted(by[k])
        print(f"  picture {k + 1} of its frame: {len(v):4d}  median {v[len(v) // 2]:5.2f}  90% {v[int(len(v) * 0.9)]:5.2f}  max {v[-1]:6.2f}  over 2 ms: {sum(1 for x in v if x > 2)}")
    print("the latest:")
    for v, i, w in sorted(worst, reverse=True)[:25]:
        print(f"  {v:6.2f} ms  id {i}  frame {w[0]} picture {w[1] + 1} of {w[2]} @{w[3]:.3f}")
