"""Compares what the IPU consumed (PS2X_IPU_IN_DUMP) with a movie's video stream (tools/pss_demux.py):
    python -I tools/ipu_dump_check.py <dump> <video.m2v>
Walks the records; bytes consumed after a CSC command (colour conversion input, not bitstream) until the next BCLR are
skipped. Reports the first place where the consumed bitstream stops matching the movie."""
import struct, sys, bisect
dump = open(sys.argv[1], 'rb').read(); m2v = open(sys.argv[2], 'rb').read()
pics = []; k = 0
while True:
    k = m2v.find(b'\x00\x00\x01\x00', k)
    if k < 0: break
    pics.append(k); k += 4
def pic(off):
    i = bisect.bisect_right(pics, off) - 1
    return f'picture {i} ({"?IPB"[(m2v[pics[i]+5]>>3)&7]}) +{off - pics[i]}' if i >= 0 else 'before the first picture'
i = 0; pos = 0; csc = False; nrec = 0; lost_events = []; mismatches = 0; last_cmds = []
def consume(data, what):
    global pos, mismatches
    if not data: return
    if m2v[pos:pos+len(data)] == data:
        pos += len(data); return
    # find first differing byte
    j = next((x for x in range(len(data)) if pos + x >= len(m2v) or m2v[pos+x] != data[x]), len(data))
    mismatches += 1
    if mismatches <= 6:
        print(f'MISMATCH #{mismatches} in a {what} record of {len(data)} bytes: stream offset {pos+j} ({pic(pos+j)}), record #{nrec}')
        print('   consumed:', data[max(0,j-8):j+24].hex(' '))
        print('   movie   :', m2v[pos+max(0,j-8):pos+j+24].hex(' '))
        # resync: where does the rest of this record occur in the movie?
        tail = data[j:j+32]
        for look in (m2v.find(tail, max(0, pos - 400000), pos + 400000),):
            if look >= 0: print(f'   the consumed bytes from there are the movie at offset {look} ({pic(look)}): {look-(pos+j):+d} bytes')
        probe = data[-48:]
        look = m2v.find(probe, max(0, pos - 400000), pos + 800000)
        if look >= 0:
            print(f'   record end resyncs at movie offset {look+48} ({look+48-(pos+len(data)):+d} bytes)')
            pos = look + 48
            return
    pos += len(data)
while i < len(dump):
    t = dump[i:i+1]; i += 1; nrec += 1
    if t == b'K':
        cmd, = struct.unpack_from('<I', dump, i); i += 4
        last_cmds.append(cmd); last_cmds[:] = last_cmds[-6:]
        code = cmd >> 28
        # CSC, PACK: picture data. SETIQ / SETVQ right after a BCLR: a table written into the FIFO (a SETIQ without one
        # reads its matrix from the bitstream itself).
        if code in (7, 8) or (code in (5, 6) and len(last_cmds) >= 2 and last_cmds[-2] >> 28 == 0): csc = True
    elif t == b'D':
        n, = struct.unpack_from('<I', dump, i); i += 4
        if not csc: consume(dump[i:i+n], 'D')
        i += n
    elif t == b'B':
        bp, n = struct.unpack_from('<II', dump, i); i += 8
        data = dump[i:i+n]; i += n
        lost, = struct.unpack_from('<I', dump, i); i += 4
        if not csc: consume(data, 'B')
        csc = False
    else:
        print('bad record', t, 'at', i); break
print(f'{nrec} records; consumed bitstream reaches movie offset {pos} of {len(m2v)} ({pic(min(pos, len(m2v)-1))}); {mismatches} mismatching records')
