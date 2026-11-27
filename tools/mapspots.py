# mapspots.py [map ...]: finds a place to stand in each of the game's maps and tries it in a hidden run.
# Needs the map_<n>.bin files in build/bin/logs/mapdump (STAR_DEBUG_MAPDUMP, see mapdump.py). For each map: the places
# of its creatures, then of its things, lifted to the player's height; one hidden run per try (the Debug page's own
# jump, STAR_DEBUG_TEST), which passes when the game names that map as the player's, the player has stopped falling
# and stands near the place asked for. Results: build/bin/logs/mapspots.txt (map x y z, or "map none").
import os, re, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapdump

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, 'build', 'bin', 'logs')
OUT = os.path.join(LOGS, 'mapspots.txt')
LIFT = 230          # the player's place is about 210 above the floor they stand on (y grows downwards)
TRIES = 14
SECONDS = '19'   # a map read from the disc for the first time can take seconds

def candidates(m, order=(2, 1)):
    _, rec, lists, _ = mapdump.read(os.path.join(LOGS, 'mapdump', 'map_%d.bin' % m))
    out = []
    for k in order:
        for r in lists[k]:
            x, y, z = mapdump.pos(r)
            if all(abs(v) < 30000 for v in (x, y, z)) and (x, y, z) != (0.0, 0.0, 0.0):
                out.append((int(round(x)), int(round(y)) - LIFT, int(round(z)), 'creature' if k == 2 else 'thing'))
    return out

def attempt(m, x, y, z):
    name = 'spot_%d' % m
    env = dict(os.environ, FROM='900', TO='900', SECONDS_LIMIT=SECONDS)
    subprocess.run(['bash', 'tools/ab.sh', name, 'q2', 'STAR_TEST_CARDS=1', 'PS2X_AUDIO_MUTE=1', 'STAR_MOUSE_KEYBOARD=1',
                    'STAR_DEBUG_WHERE=15', 'STAR_DEBUG_TEST=60:go:%d:%d:%d:%d' % (m, x, y, z)],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    seen = []
    with open(os.path.join(LOGS, 'ab_%s.log' % name), 'rb') as f:
        text = f.read().decode('latin1')
        if 'STA_GO.ADX' in text:
            # the game-over music: the player, standing still, was killed (a creature's place is where it attacks;
            # the Moving Block Area crushes)
            return None, 'the player died'
        for line in text.splitlines():
            g = re.match(r'\[STAR debug\] frame (\d+): map (\d+) at (-?\d+) (-?[\d.]+) (-?\d+)', line)
            if g and int(g.group(1)) > 60:
                seen.append((int(g.group(1)), int(g.group(2)), int(g.group(3)), float(g.group(4)), int(g.group(5))))
    if len(seen) < 6:
        return None, 'the game stopped (%d reports after the jump)' % len(seen)
    last = seen[-4:]
    if any(s[1] != m for s in last):
        return None, 'the game names map %d' % last[-1][1]
    if max(s[3] for s in last) - min(s[3] for s in last) > 1.0:
        return None, 'still falling (y %.0f to %.0f)' % (last[0][3], last[-1][3])
    if abs(last[-1][2] - x) > 400 or abs(last[-1][4] - z) > 400:
        return None, 'ended far away (%d %d)' % (last[-1][2], last[-1][4])
    return (last[-1][2], int(round(last[-1][3])), last[-1][4]), 'stands at %d %.0f %d after %d frames' % (last[-1][2], last[-1][3], last[-1][4], last[-1][0] - 60)

def main():
    done = {}
    if os.path.exists(OUT):
        for line in open(OUT):
            p = line.split()
            if len(p) == 4:
                done[int(p[0])] = tuple(int(v) for v in p[1:])
    if sys.argv[1:2] == ['--verify']:
        # every place kept so far, tried as it is: it stays only if the player stands there
        only = [int(v) for v in sys.argv[2:]]
        for m in [k for k in sorted(done) if not only or k in only]:
            x, y, z = done[m]
            spot, why = attempt(m, x, y, z)
            print('map %d: kept place %d %d %d: %s' % (m, x, y, z, why), flush=True)
            if not spot or abs(spot[1] - y) > 40 or abs(spot[0] - x) > 40 or abs(spot[2] - z) > 40:
                print('map %d: NOT kept' % m, flush=True)
                del done[m]
                with open(OUT, 'w') as f:
                    for k in sorted(done):
                        f.write('%d %d %d %d\n' % (k, *done[k]))
        return
    if sys.argv[1:2] == ['--calm']:
        # other places for these maps: where things are first, then creatures (a player who stands still must live)
        for m in [int(a) for a in sys.argv[2:]]:
            done.pop(m, None)
            for (x, y, z, kind) in candidates(m, (1, 2))[:24]:
                spot, why = attempt(m, x, y, z)
                print('map %d: %s at %d %d %d: %s' % (m, kind, x, y, z, why), flush=True)
                if spot:
                    # and once more from where the player came to rest (the Moving Block Area kills by the clock)
                    again, why = attempt(m, spot[0], spot[1] - 8, spot[2])
                    print('map %d:    again at %d %d %d: %s' % (m, spot[0], spot[1] - 8, spot[2], why), flush=True)
                    if again:
                        done[m] = (spot[0], spot[1] - 8, spot[2])
                        break
            with open(OUT, 'w') as f:
                for k in sorted(done):
                    f.write('%d %d %d %d' % (k, *done[k]) + chr(10))
        return
    maps = [int(a) for a in sys.argv[1:]] or sorted(int(n[4:-4]) for n in os.listdir(os.path.join(LOGS, 'mapdump')) if n.startswith('map_'))
    for m in maps:
        if m in done and len(sys.argv) == 1:
            continue
        found = None
        for (x, y, z, kind) in candidates(m)[:TRIES]:
            spot, why = attempt(m, x, y, z)
            print('map %d: %s at %d %d %d: %s' % (m, kind, x, y, z, why), flush=True)
            if spot:
                found = (spot[0], spot[1] - 8, spot[2])   # where the player came to rest, a hand's breadth higher
                break
        if found:
            done[m] = found
        with open(OUT, 'w') as f:
            for k in sorted(done):
                f.write('%d %d %d %d\n' % (k, *done[k]))

if __name__ == '__main__':
    main()
