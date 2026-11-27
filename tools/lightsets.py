"""lightsets.py <ee memory image>: light blocks in EE RAM (the rows the game sends to VU1 q43..q52), distinct ones."""
import sys, struct
from collections import OrderedDict
d = open(sys.argv[1], 'rb').read()
one = struct.pack('<4f', 1.0, 0.0, 0.0, 0.0)
seen = OrderedDict()
pos = 0
while True:
    pos = d.find(one + one, pos)          # q46 and q47 = (1,0,0,0)
    if pos < 0: break
    if pos % 4 == 0 and pos >= 48:
        f = struct.unpack_from('<40f', d, pos - 48)   # q43..q52
        q44, q45 = f[4:8], f[8:12]
        l0 = sum(v * v for v in q44[:3]); l1 = sum(v * v for v in q45[:3])
        if 0.98 < l0 < 1.02 and 0.98 < l1 < 1.02:
            key = tuple(round(v, 3) for v in f[0:4] + f[4:12] + f[20:24] + f[36:40])
            seen.setdefault(key, []).append(pos - 48)
    pos += 4
print("distinct light blocks:", len(seen))
for key, where in seen.items():
    print(f"x{len(where):3d} first at 0x{where[0]:07x}  flags {key[0:4]}  dir0 {key[4:8]}  dir1 {key[8:12]}  colour {key[12:16]}  ambient {key[16:20]}")
