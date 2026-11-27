"""vutrace.py: parses a PS2X_VU1_TRACE file. load(path) -> list of dicts {pc, itop, top, vi, vf, data(bytes 16K, the VU1 data memory at the call)}."""
import struct
MAGIC = b'V1TR'
def find_state_size(d):
    for S in range(700, 20000, 4):
        off = 0; ok = 0
        while off < len(d) and ok < 6:
            if d[off:off+4] != MAGIC: break
            p = off + 24 + S
            good = False
            while p + 4 <= len(d):
                idx, cnt = struct.unpack_from('<HH', d, p); p += 4
                if idx == 0xFFFF and cnt == 0: good = True; break
                if idx + cnt > 1024: break
                p += cnt * 16
            if not good: break
            off = p; ok += 1
        if ok >= 6 or (ok and off == len(d)): return S
    return None
def load(path):
    d = open(path, 'rb').read()
    S = find_state_size(d)
    out = []; mem = bytearray(0x4000); off = 0
    while off < len(d):
        assert d[off:off+4] == MAGIC, off
        st = off + 24
        vf = struct.unpack_from('<128f', d, st)
        vi = struct.unpack_from('<16i', d, st + 512)
        pc, = struct.unpack_from('<I', d, st + 608)
        p = st + S
        diffs = []
        while True:
            idx, cnt = struct.unpack_from('<HH', d, p); p += 4
            if idx == 0xFFFF: break
            diffs.append((idx, cnt))
            mem[idx*16:(idx+cnt)*16] = d[p:p+cnt*16]; p += cnt * 16
        out.append(dict(pc=pc, vi=vi, vf=vf, data=bytes(mem), state=d[st:st+S], diffs=diffs))
        off = p
    return out, S
if __name__ == '__main__':
    import sys
    from collections import Counter
    recs, S = load(sys.argv[1])
    print("state size", S, "records", len(recs))
    print("start pcs:", Counter(hex(r['pc']) for r in recs).most_common(12))
