"""vifdump.py <file from PS2X_VIF_DUMP> [--all]: decodes the VIF1 items of a dumped frame.
Prints the commands that matter for the camera (UNPACKs to VU1 addresses below 24 without the TOPS flag, MSCALs counted),
or every command with --all (long)."""
import struct, sys
from collections import Counter

NAMES = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD', 0x06: 'MSKPATH3', 0x07: 'MARK',
         0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT', 0x20: 'STMASK',
         0x30: 'STROW', 0x31: 'STCOL', 0x4A: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}


def items(path):
    d = open(path, 'rb').read()
    off = 0
    while off < len(d):
        kind, size = struct.unpack_from('<II', d, off)
        yield kind, d[off + 8:off + 8 + size]
        off += 8 + size


def commands(data):
    """Yields (offset, name, num, imm, payload bytes, extra dict) for a VIF1 byte stream."""
    cl, wl = 1, 1
    p = 0
    n = len(data) & ~3
    while p < n:
        code, = struct.unpack_from('<I', data, p)
        cmd = (code >> 24) & 0x7F
        num = (code >> 16) & 0xFF
        imm = code & 0xFFFF
        start = p
        p += 4
        extra = {}
        if cmd == 0x01:
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
            size = 0
        elif cmd == 0x20:
            size = 4
        elif cmd in (0x30, 0x31):
            size = 16
        elif cmd == 0x4A:
            size = (num or 256) * 8
        elif cmd in (0x50, 0x51):
            size = (imm or 65536) * 16
        elif cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            count = num or 256
            if wl <= cl or wl == 0:
                vectors = count
            else:
                vectors = cl * (count // wl) + min(count % wl, cl)
            bits = (32 >> vl) * (vn + 1)
            if vn == 3 and vl == 3:
                bits = 16
            size = ((vectors * bits + 31) // 32) * 4
            extra = dict(vn=vn, vl=vl, addr=imm & 0x3FF, tops=bool(imm & 0x8000), usn=bool(imm & 0x4000), mask=bool(cmd & 0x10),
                         cl=cl, wl=wl, count=count)
        else:
            size = 0
        name = NAMES.get(cmd, 'UNPACK' if cmd >= 0x60 else f'?{cmd:02x}')
        yield start, name, num, imm, data[p:p + size], extra
        p += size


if __name__ == '__main__':
    show_all = '--all' in sys.argv
    total = Counter()
    for index, (kind, data) in enumerate(items(sys.argv[1])):
        if kind == 0:
            total['GIF path 3 items'] += 1
            total['GIF path 3 bytes'] += len(data)
            continue
        total['VIF1 items'] += 1
        total['VIF1 bytes'] += len(data)
        for off, name, num, imm, payload, extra in commands(data):
            total[name] += 1
            if name == 'UNPACK' and not extra['tops'] and extra['addr'] < 24 or show_all:
                text = f"item {index} +{off:#x} {name} num={num} imm={imm:#x}"
                if extra:
                    text += f" addr={extra['addr']} tops={int(extra['tops'])} v{extra['vn'] + 1}-{32 >> extra['vl']} cl/wl={extra['cl']}/{extra['wl']}"
                    if extra['vn'] == 3 and extra['vl'] == 0:
                        f = struct.unpack(f"<{len(payload) // 4}f", payload)
                        text += ' | ' + ' '.join(f"{v:.3f}" for v in f[:8]) + (' ...' if len(f) > 8 else '')
                print(text)
    print(dict(total))
