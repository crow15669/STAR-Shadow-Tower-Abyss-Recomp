"""hit.py <trace> <x> <y> [radius]: the draws of a PS2X_GS_TRACE file whose triangles/sprites cover screen point (x, y)."""
import sys, re
path, px, py = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
rad = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
def inside(p, a, b, c):
    def s(u, v, w): return (u[0]-w[0])*(v[1]-w[1]) - (v[0]-w[0])*(u[1]-w[1])
    d1, d2, d3 = s(p, a, b), s(p, b, c), s(p, c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)
n = 0
for ln, line in enumerate(open(path, errors='replace'), 1):
    if ' D prim=' not in line:
        continue
    n += 1
    parts = line.rstrip('\n').split(' | ')
    head = parts[0]
    prim = int(re.search(r'prim=(\d+)', head).group(1))
    ofs = re.search(r'ofs=(\d+),(\d+)', head)
    ox, oy = int(ofs.group(1)) / 16.0, int(ofs.group(2)) / 16.0
    vs = []
    for v in parts[1:]:
        m = re.match(r'\s*([-\d.]+),([-\d.]+) z=(\d+) rgba=([0-9a-f]+)', v)
        if m:
            vs.append((float(m.group(1)) - ox, float(m.group(2)) - oy, int(m.group(3)), m.group(4), v))
    hit = None
    if prim == 6:
        for i in range(0, len(vs) - 1, 2):
            a, b = vs[i], vs[i + 1]
            if min(a[0], b[0]) - rad <= px <= max(a[0], b[0]) + rad and min(a[1], b[1]) - rad <= py <= max(a[1], b[1]) + rad:
                hit = (a, b)
    elif prim in (3, 4, 5):
        tris = []
        if prim == 3:
            tris = [(vs[i], vs[i+1], vs[i+2]) for i in range(0, len(vs) - 2, 3)]
        elif prim == 4:
            tris = [(vs[i], vs[i+1], vs[i+2]) for i in range(len(vs) - 2)]
        else:
            tris = [(vs[0], vs[i], vs[i+1]) for i in range(1, len(vs) - 1)]
        for t in tris:
            if inside((px, py), t[0], t[1], t[2]):
                hit = t
                break
    if hit:
        keep = re.findall(r'(prim=\d+|iip=\d|tme=\d|abe=\d|fst=\d|frame=\S+|zbuf=\S+|tex0=\S+|alpha=\S+|test=\S+|fba=\d)', head)
        print(f"#{n} line {ln} verts {len(vs)}: " + ' '.join(keep))
        for v in hit:
            print(f"      {v[0]:.1f},{v[1]:.1f} z={v[2]} rgba={v[3]}")
print("draws:", n)
