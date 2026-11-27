"""Import stubs of a dumped IOP module image (PS2X_IOP_DUMP_MODULES): python -I tools/irx_imports.py <irx_NN_BASE.bin>
Prints address -> library:ordinal for every stub (an import table starts with 0x41E00000; stubs are jr ra / li zero,N)."""
import struct, sys, re
path = sys.argv[1]
base = int(re.search(r'_([0-9a-f]{6})\.bin$', path).group(1), 16)
d = open(path, 'rb').read()
for off in range(0, len(d) - 20, 4):
    if struct.unpack_from('<I', d, off)[0] == 0x41E00000 and struct.unpack_from('<I', d, off + 4)[0] == 0:
        name = d[off + 12:off + 20].split(b'\0')[0].decode('ascii', 'replace')
        p = off + 20
        while p + 8 <= len(d):
            a, b = struct.unpack_from('<II', d, p)
            if a != 0x03E00008 or (b >> 16) != 0x2400:
                break
            print(f'0x{base + p:06x} {name}:{b & 0xFFFF}')
            p += 8
