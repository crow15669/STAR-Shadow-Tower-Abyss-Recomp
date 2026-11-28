#!/usr/bin/env python3
"""Merge GS warm-up lists (thirdparty/pcsx2-gs/compat/GSWarmList.h) into the list GHR ships.

The GS learns every pipeline (Vulkan) or shader (Direct3D 11) it builds into <cache>/ghr_warm_vulkan.bin /
ghr_warm_dx11.bin (the play folder's shadercache_pcsx2). This unions such files, from any number of PCs and GPUs,
into resources/gs_warm/vulkan.bin or dx11.bin: one record per (GPU tag, selector), sorted, so a rerun with the same
inputs writes the same bytes.

    python3 -I tools/gs/warm_merge.py resources/gs_warm/vulkan.bin <learned or shipped files...>

The output may be one of the inputs (the shipped list grows). Files of another version, API or entry size stop the
merge with a message. Prints the records per GPU tag.
"""
import struct
import sys
from collections import Counter
from pathlib import Path

HEADER = struct.Struct("<4sIII")  # magic "GHRW", version, entry size, api (1 Vulkan, 2 Direct3D 11)
API_NAMES = {1: "Vulkan", 2: "Direct3D 11"}


def read(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise SystemExit(f"{path}: too short for a warm-up list")
    magic, version, size, api = HEADER.unpack_from(data)
    if magic != b"GHRW":
        raise SystemExit(f"{path}: not a warm-up list")
    record = 8 + size
    body = data[HEADER.size:]
    whole = len(body) - len(body) % record  # a record cut short by a crash is dropped
    records = {body[i:i + record] for i in range(0, whole, record)}
    return (version, size, api), records


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    out = Path(argv[1])
    inputs = [Path(p) for p in argv[2:]]
    if out.exists() and out not in inputs:
        inputs.insert(0, out)
    kind = None
    merged = set()
    for path in inputs:
        this, records = read(path)
        if kind is None:
            kind = this
        elif this != kind:
            raise SystemExit(f"{path}: version / entry size / api {this} differs from {kind}")
        merged |= records
        print(f"{path}: {len(records)} records")
    version, size, api = kind
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(HEADER.pack(b"GHRW", version, size, api) + b"".join(sorted(merged)))
    tags = Counter(struct.unpack_from("<Q", r)[0] for r in merged)
    print(f"{out}: {len(merged)} {API_NAMES.get(api, api)} records, version {version}")
    for tag, count in sorted(tags.items()):
        print(f"  GPU tag {tag:016X}: {count}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
