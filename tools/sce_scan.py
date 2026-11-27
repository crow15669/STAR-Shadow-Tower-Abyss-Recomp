"""Name Sony SDK functions in a stripped PS2 program with Aura-Decomp-Tool's SCE fingerprint database.

    python -I tools/sce_scan.py <elf> <symbols.json> <out.csv>

symbols.json (github.com/danny199012/Aura-Decomp-Tool, src-tauri/resources/sce_sdk, GPL-3.0) holds, per library and
function, the SHA-1 of the function's body with its relocated words masked (MIPS_26: opcode kept; HI16/LO16: upper
half kept; MIPS_32: zero), its size and its relocations. Same check as Aura's sce_symbol_scanner.rs (matches_symbol):
mask the candidate body the same way, hash, compare. Candidates come from an index of the program's word pairs at each
record's first two unrelocated words.

Output CSV: Name,Address,Size,Library,SDK (the SDK versions the body occurs in), one row per match.
"""
import csv
import hashlib
import json
import struct
import sys
from collections import defaultdict


def load_text(path):
    data = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from('<8I', data, phoff + i * phentsize)
        if p_type == 1 and p_filesz:
            return p_vaddr, data[p_offset:p_offset + p_filesz]   # PS2 boot ELFs: one loadable segment
    raise SystemExit('no loadable segment')


def mask_word(rtype, value):
    if rtype in ('MIPS_26', 'MIPS26'):
        return value & 0xFC000000
    if rtype in ('MIPS_HI16', 'MIPS_LO16', 'HI16', 'LO16', 'MIPSHI16', 'MIPSLO16', 'MIPS_GPREL16', 'GPREL16'):
        return value & 0xFFFF0000
    if rtype in ('MIPS_32', 'MIPS32'):
        return 0
    return value


def main():
    elf_path, db_path, out_path = sys.argv[1:4]
    base, text = load_text(elf_path)
    nwords = len(text) // 4
    words = struct.unpack_from(f'<{nwords}I', text)
    pairs = defaultdict(list)
    for i in range(nwords - 1):
        pairs[(words[i], words[i + 1])].append(i)

    db = json.load(open(db_path, encoding='utf-8'))
    records = 0
    found = []
    for library, functions in db.items():
        for name, by_hash in functions.items():
            for sha1, variants in by_hash.items():
                for _, entry in variants.items():
                    if entry.get('type') != 'FUNCTION':
                        continue
                    records += 1
                    size = entry['size']
                    if size < 8 or size % 4:
                        continue
                    relocs = {int(off): r['type'] for off, r in (entry.get('relocations') or {}).items()}
                    # The record's static words are unknown (only the hash is stored): anchor on the program instead.
                    # Every position whose masked body hashes right is a match; candidates are the positions where two
                    # consecutive unrelocated words could start, i.e. all of them, filtered by size bounds. To keep it
                    # fast, hash only positions that are function-like (preceded by jr ra / a nop or at the start).
                    found.append((library, name, sha1, size, relocs, entry.get('sdk') or []))
    print(f'{records} function records; scanning {nwords} words', flush=True)

    # Positions where a function can start: after `jr ra` + delay slot, after a nop run, or after a `j` + delay slot.
    starts = set([0])
    for i in range(nwords - 2):
        w = words[i]
        if w == 0x03E00008 or (w >> 26) == 0x02:      # jr ra / j: the function ends after the delay slot
            starts.add(i + 2)
        if w == 0 and i + 1 < nwords and words[i + 1] != 0:
            starts.add(i + 1)
    starts = sorted(s for s in starts if s < nwords)
    print(f'{len(starts)} candidate start positions', flush=True)

    # Group records by size so each candidate start is hashed once per distinct (size, relocation layout).
    layouts = defaultdict(list)
    for library, name, sha1, size, relocs, sdk in found:
        layouts[(size, tuple(sorted(relocs.items())))].append((sha1, library, name, sdk))

    out_rows = []
    for (size, relocs), recs in layouts.items():
        by_sha = defaultdict(list)
        for sha1, library, name, sdk in recs:
            by_sha[sha1].append((library, name, sdk))
        n = size // 4
        rel = dict(relocs)
        for s in starts:
            if s + n > nwords:
                break
            body = list(words[s:s + n])
            for off, rtype in rel.items():
                k = off // 4
                if k < n:
                    body[k] = mask_word(rtype, body[k])
            digest = hashlib.sha1(struct.pack(f'<{n}I', *body)).hexdigest()
            if digest in by_sha:
                for library, name, sdk in by_sha[digest]:
                    out_rows.append((name, base + 4 * s, size, library, ' '.join(sdk[:3]) + (' ...' if len(sdk) > 3 else '')))

    out_rows.sort(key=lambda r: (r[1], -r[2]))
    with open(out_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Name', 'Address', 'Size', 'Library', 'SDK'])
        for name, addr, size, library, sdk in out_rows:
            w.writerow([name, f'0x{addr:08X}', size, library, sdk])
    print(f'{len(out_rows)} matches at {len({r[1] for r in out_rows})} addresses -> {out_path}')


if __name__ == '__main__':
    main()
