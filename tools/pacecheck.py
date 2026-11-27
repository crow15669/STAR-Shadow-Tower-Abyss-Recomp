"""pacecheck.py <pace log> [skip seconds]: how evenly the ordinary pictures (menus, logos, movies) were laid on the
screen's refreshes, from a PS2X_PCSX2_PACE_LOG file. Works in hidden runs too (STAR_REFRESH_HZ=<rate> pretends a
screen). Prints the steps between consecutive pictures in refreshes (the row should be the most even one the two
rates allow), how many were left out, how late the presents were made and how far the fields came off the even line."""
import sys, csv
rows = list(csv.DictReader(open(sys.argv[1])))
skip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
if not rows:
    print('empty log')
    sys.exit(0)
t0 = int(rows[0]['now'])
rows = [r for r in rows if (int(r['now']) - t0) / 1e9 >= skip]
q = [r for r in rows if r['kind'] == 'q']
left_out = sum(1 for r in rows if r['kind'] == 's')
p = [r for r in rows if r['kind'] == 'p']
if len(q) < 3:
    print('too few pictures')
    sys.exit(0)
period = int(q[-1]['period'])
steps = {}
row = []
for r in q[1:]:
    n = int(r['interval'])
    steps[n] = steps.get(n, 0) + 1
    row.append(n)
seconds = (int(q[-1]['now']) - int(q[0]['now'])) / 1e9
print(f'screen {1e9 / period:.2f} Hz, {len(q)} pictures in {seconds:.1f} s ({len(q) / max(seconds, 1e-9):.2f} a second), {left_out} left out')
print('steps in refreshes: ' + ', '.join(f'{k}: {v}' for k, v in sorted(steps.items())))
ideal = (int(q[-1]['target']) - int(q[0]['target'])) / period / (len(q) - 1)
print(f'mean step {ideal:.4f} refreshes; the most even row has only {int(ideal)} and {int(ideal) + 1}' if ideal != int(ideal) else f'mean step {ideal:.0f}')
print('the row, first 60: ' + ''.join(str(min(n, 9)) for n in row[:60]))
# the whole row in runs ("1x498 2 1x501": one doubled picture between two long even stretches; x = a picture left out)
seq = []
for r in rows:
    if r['kind'] == 'q':
        seq.append(str(min(int(r['interval']), 9)))
    elif r['kind'] == 's':
        seq.append('x')
runs = []
for c in seq:
    if runs and runs[-1][0] == c:
        runs[-1][1] += 1
    else:
        runs.append([c, 1])
if len(runs) <= 40:
    print('in runs: ' + ' '.join(c if n == 1 else f'{c}x{n}' for c, n in runs))
# runs of the row that are not the two neighbouring step sizes
odd = sum(v for k, v in steps.items() if k not in (int(ideal), int(ideal) + 1))
print(f'steps outside those two: {odd}')
off = sorted(abs(float(r['off_line'])) / 1e6 for r in q)
print(f'fields off the even line: median {off[len(off) // 2]:.2f} ms, 99% under {off[int(len(off) * 0.99)]:.2f} ms, worst {off[-1]:.2f} ms')
# a present made after its refresh's turn began (time + period - 0.5 ms = the refresh it was for)
late = [(int(r['now']) - int(r['time'])) / 1e6 for r in p]
if late:
    late.sort()
    missed = sum(1 for x in late if x > period / 1e6 - 0.5)
    print(f'presents: {len(p)}; made after their time by median {late[len(late) // 2]:.2f} ms, 99% under {late[int(len(late) * 0.99)]:.2f} ms, '
          f'worst {late[-1]:.2f} ms; {missed} made after their refresh had passed')
