# mapdump.py <folder> [map ...]: reads the map_<n>.bin files written with STAR_DEBUG_MAPDUMP (debug_menu.cpp): a map's
# record (0x1e0 bytes), its parts (0x30 each), things (0x30), creatures (0x54), people (0x34), then 0x120 bytes of links.
import struct, sys, os, glob

def read(path):
    b = open(path, 'rb').read()
    rec = b[:0x1e0]
    n = struct.unpack('<4H', rec[0x6c:0x74])
    sizes = (0x30, 0x30, 0x54, 0x34)
    at = 0x1e0
    lists = []
    for k in range(4):
        lists.append([b[at + i * sizes[k]: at + (i + 1) * sizes[k]] for i in range(n[k])])
        at += n[k] * sizes[k]
    links = b[at:at + 0x120]
    return struct.unpack('<I', rec[:4])[0], rec, lists, links

def pos(r):
    return struct.unpack('<3f', r[:12])

if __name__ == '__main__':
    folder = sys.argv[1]
    want = [int(a) for a in sys.argv[2:]]
    for path in sorted(glob.glob(os.path.join(folder, 'map_*.bin')), key=lambda p: int(os.path.basename(p)[4:-4])):
        m, rec, lists, links = read(path)
        if want and m not in want:
            continue
        print('map %d: %d parts, %d things, %d creatures, %d people' % (m, *[len(l) for l in lists]))
        for name, k in (('thing', 1), ('creature', 2), ('person', 3)):
            for r in lists[k]:
                kind, number = struct.unpack('<2H', r[0x18:0x1c])
                print('   %-8s kind %3d no %4d at %8.0f %8.0f %8.0f   %s' % (name, kind, number, *pos(r), r[0x1c:0x30].hex(' ', 2)))
        for i in range(8):
            l = struct.unpack('<H3h', links[0x36 + i * 8: 0x3e + i * 8])
            if l[0] and l[0] < 256:
                print('   link to map %d, its place %d %d %d' % l)
