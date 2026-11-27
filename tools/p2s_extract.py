"""p2s_extract.py <state.p2s> <out dir>: extracts a PCSX2 save state (zip with zstd entries). Needs the zstandard module."""
import sys, os, struct, zlib, zipfile, zstandard
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
z = zipfile.ZipFile(src)
raw = open(src, 'rb')
for info in z.infolist():
    raw.seek(info.header_offset)
    h = raw.read(30)
    nlen, elen = struct.unpack_from('<HH', h, 26)
    raw.seek(info.header_offset + 30 + nlen + elen)
    data = raw.read(info.compress_size)
    if info.compress_type == 93:
        data = zstandard.ZstdDecompressor().decompress(data, max_output_size=info.file_size)
    elif info.compress_type == 8:
        data = zlib.decompress(data, -15)
    open(os.path.join(out, info.filename), 'wb').write(data)
    print(info.filename, len(data))
