"""Video elementary stream of a PSS movie (MPEG-2 program stream): python -I tools/pss_demux.py <in.pss> <out.m2v>
Also prints the sequence header's size and frame rate, and the audio stream's packets (private stream 1)."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
out = bytearray(); i = 0; n = len(d); audio = 0
while i + 4 <= n:
    if d[i:i+3] != b'\x00\x00\x01':
        i += 1; continue
    sid = d[i+3]
    if sid == 0xBA:                      # pack header (MPEG-2: 14 bytes + stuffing)
        i += 14 + (d[i+13] & 7) if (d[i+4] & 0xC0) == 0x40 else 12
    elif sid == 0xB9:
        i += 4
    elif sid >= 0xBB:
        ln = struct.unpack_from('>H', d, i+4)[0]
        if 0xE0 <= sid <= 0xEF:
            hl = d[i+8]
            out += d[i+9+hl:i+6+ln]
        elif sid == 0xBD:
            audio += 1
        i += 6 + ln
    else:
        i += 4
open(sys.argv[2], 'wb').write(out)
k = out.find(b'\x00\x00\x01\xb3')
w = (out[k+4] << 4) | (out[k+5] >> 4); h = ((out[k+5] & 15) << 8) | out[k+6]
print(f'{len(out):,} bytes of video; sequence header at {k}: {w}x{h}, frame rate code {out[k+7] & 15}, bitrate {((out[k+8]<<10)|(out[k+9]<<2)|(out[k+10]>>6))*400/1e6:.2f} Mbit/s; {audio} audio packets')
pics = out.count(b'\x00\x00\x01\x00'); print(pics, 'pictures')
