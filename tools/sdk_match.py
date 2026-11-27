"""Name Shadow Tower Abyss's library functions by matching them against God Hand's named functions.

Both games are built with Sony's PS2 libraries (and the same compiler family), so a library function such as
sceCdRead is usually the same code in both programs, apart from the addresses it calls and the data it points at.
This script blanks those parts out of every instruction (call/jump targets, upper halves of addresses, and the
immediate of any load/store/addiu that is not relative to the stack pointer), then looks for each named God Hand
function in Shadow Tower's program.

Usage:
  python -I sdk_match.py <named ELF> <names CSV: Name,Start,End,Size> <unnamed ELF> <out CSV> [--min-words N]

Output CSV: Name,Address,Words,SourceAddress,Matches (Matches > 1 means the same code sits in several places).
"""
import csv, struct, sys
from collections import defaultdict

def load_elf(path):
    """Returns (base, words) for every PT_LOAD segment with file data: a list of (vaddr, bytes)."""
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF', path
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    segs = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from('<8I', data, phoff + i * phentsize)
        if p_type == 1 and p_filesz:
            segs.append((p_vaddr, data[p_offset:p_offset + p_filesz], p_flags))
    return segs

def words_of(seg_bytes):
    n = len(seg_bytes) // 4
    return list(struct.unpack_from(f'<{n}I', seg_bytes, 0))

SP = 29
LOADSTORE_ADDIU = {0x08, 0x09, 0x0D, 0x18, 0x19, 0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                   0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x31, 0x35, 0x36, 0x37, 0x39, 0x3D, 0x3E, 0x3F}

def mask(w):
    op = w >> 26
    if op in (0x02, 0x03):              # j / jal: the target moves
        return w & 0xFC000000
    if op == 0x0F:                      # lui: upper half of an address (or a constant: masked anyway)
        return w & 0xFFFF0000
    if op in LOADSTORE_ADDIU and ((w >> 21) & 31) != SP:
        return w & 0xFFFF0000           # %lo of an address (gp-relative too)
    return w

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    min_words = 8
    if '--min-words' in sys.argv:
        min_words = int(sys.argv[sys.argv.index('--min-words') + 1])
    named_elf, names_csv, target_elf, out_csv = args

    # Source: named functions.
    src_segs = load_elf(named_elf)
    def src_words(start, end):
        for base, b, _ in src_segs:
            if base <= start and end <= base + len(b):
                return words_of(b[start - base:end - base])
        return None
    funcs = []
    with open(names_csv, newline='') as f:
        for row in csv.DictReader(f):
            name = row['Name']
            if name.startswith(('func_', 'FUN_', 'entry_', 'sub_', 'LAB_')):
                continue
            start, end = int(row['Start'], 16), int(row['End'], 16)
            w = src_words(start, end)
            if w and len(w) >= min_words:
                funcs.append((name, start, [mask(x) for x in w]))

    # Target: every word position of every loaded segment, indexed by its first 4 masked words.
    tgt_words, tgt_addr = [], []
    for base, b, _ in load_elf(target_elf):
        ws = words_of(b)
        tgt_words.extend(mask(x) for x in ws)
        tgt_addr.extend(base + 4 * i for i in range(len(ws)))
    index = defaultdict(list)
    for i in range(len(tgt_words) - 3):
        index[tuple(tgt_words[i:i + 4])].append(i)

    results, by_addr = [], defaultdict(list)
    for name, start, mw in funcs:
        hits = [i for i in index.get(tuple(mw[:4]), ()) if tgt_words[i:i + len(mw)] == mw]
        if hits:
            results.append((name, start, len(mw), hits))
            for i in hits:
                by_addr[tgt_addr[i]].append((len(mw), name))

    # One name per target address: the longest source function wins (ties: keep all for review).
    with open(out_csv, 'w', newline='') as f:
        out = csv.writer(f)
        out.writerow(['Name', 'Address', 'Words', 'SourceAddress', 'Matches'])
        for name, start, n, hits in sorted(results, key=lambda r: tgt_addr[r[3][0]]):
            for i in hits:
                best = max(by_addr[tgt_addr[i]])[0]
                if n == best:
                    out.writerow([name, f'0x{tgt_addr[i]:08X}', n, f'0x{start:08X}', len(hits)])
    unique_addrs = len(by_addr)
    print(f'{len(funcs)} named source functions of {min_words}+ words; '
          f'{len(results)} found in the target at {unique_addrs} addresses -> {out_csv}')

if __name__ == '__main__':
    main()
