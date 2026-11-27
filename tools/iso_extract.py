"""Copy one file out of a PS2 disc image (ISO 9660, root folder or a subfolder path like TARGET/STAZ.STZ)."""
import struct, sys
def main(iso_path, name, out_path):
    f = open(iso_path, 'rb')
    f.seek(16 * 2048); pvd = f.read(2048)
    lba, size = struct.unpack('<I', pvd[158:162])[0], struct.unpack('<I', pvd[166:170])[0]
    for part in name.strip('/').split('/'):
        f.seek(lba * 2048); d = f.read(size); i = 0; found = None
        while i < len(d):
            l = d[i]
            if l == 0:
                i = (i // 2048 + 1) * 2048; continue
            e = d[i:i + l]; nm = e[33:33 + e[32]].decode('ascii', 'replace').split(';')[0]
            if nm.upper() == part.upper():
                found = struct.unpack('<I', e[2:6])[0], struct.unpack('<I', e[10:14])[0]; break
            i += l
        if not found: sys.exit(f'{part} not found')
        lba, size = found
    f.seek(lba * 2048); data = f.read(size)
    open(out_path, 'wb').write(data); print(f'{name}: {size:,} bytes -> {out_path}')
if __name__ == '__main__':
    main(*sys.argv[1:4])
