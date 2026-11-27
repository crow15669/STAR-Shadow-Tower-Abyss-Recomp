"""Code addresses the game can reach through a pointer, that are not function starts in recomp/sta_functions.csv.

    python -I tools/find_code_pointers.py   (reads game/SLPS_252.17, writes recomp/sta_entry_points.csv)

Two sources:
  data   a 32-bit word in the program's data (after the code) whose value is a 4-aligned code address:
         function-pointer tables, vtables, callbacks in structs, and switch jump tables
  lui    a lui/addiu (or lui/ori) pair in code that builds a code address (a callback passed to a function)
The recompiler takes them as entry_points hints (tools/make_config.py): inside a known function they become extra
entries into it, elsewhere small standalone functions. A switch target in the list does no harm: it only becomes
reachable through the dispatch table too.
"""
import csv
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    d = (ROOT / "game" / "SLPS_252.17").read_bytes()
    phoff, = struct.unpack_from("<I", d, 28)
    _, off, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", d, phoff)
    words = struct.unpack_from(f"<{filesz // 4}I", d, off)

    def word(a):
        return words[(a - vaddr) // 4]

    starts, code_end, ranges = set(), 0, []
    with open(ROOT / "recomp" / "sta_functions.csv", newline="") as f:
        for r in csv.DictReader(f):
            s, e = int(r["Start"], 16), int(r["End"], 16)
            if not r["Name"].startswith("entry_"):
                starts.add(s)
            ranges.append((s, e))
            code_end = max(code_end, e)
    ranges.sort()
    import bisect
    range_starts = [s for s, _ in ranges]

    def in_known_code(a):
        i = bisect.bisect_right(range_starts, a) - 1
        while i >= 0 and ranges[i][0] <= a:
            if a < ranges[i][1]:
                return True
            i -= 1
            if a - ranges[i + 1][0] > 0x10000:
                break
        return False

    def after_return(a):
        # Skip nop padding back to a `jr ra` or `j` and its delay slot.
        b = a - 4
        while b >= vaddr and word(b) == 0 and a - b < 64:
            b -= 4
        # b is the last non-nop word: the delay slot of the return, or the return itself if its delay slot was a nop.
        return word(b) == 0x03E00008 or word(b) >> 26 == 0x02 or word(b - 4) == 0x03E00008 or word(b - 4) >> 26 == 0x02

    found = {}  # target -> (kind, first source)

    def add(target, kind, source):
        if not (0x100000 <= target < code_end and target % 4 == 0) or target in starts or target in found:
            return
        # Pointers into data kept inside the code range (tables, kernel patch blobs) are not code.
        if in_known_code(target) or after_return(target):
            found[target] = (kind, source)

    for a in range(code_end, vaddr + filesz, 4):
        add(word(a), "data", a)

    for a in range(vaddr, code_end - 16, 4):
        w = word(a)
        if w >> 26 != 0x0F:                      # lui rt, hi
            continue
        reg, hi = (w >> 16) & 31, (w & 0xFFFF) << 16
        for k in range(1, 5):
            x = word(a + 4 * k)
            op, rs, rt, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
            if op in (0x09, 0x0D) and rs == reg:  # addiu / ori with the lui's register
                add((hi + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)) & 0xFFFFFFFF
                    if op == 0x09 else hi | imm, "lui", a)
                break
            if (op == 0 and ((x >> 11) & 31) == reg) or (op not in (0, 2, 3, 4, 5) and rt == reg and op < 0x28):
                break                             # the register was overwritten

    with open(ROOT / "recomp" / "sta_entry_points.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Address", "Kind", "Source"])
        for target in sorted(found):
            kind, source = found[target]
            w.writerow([f"0x{target:08X}", kind, f"0x{source:08X}"])
    kinds = {}
    for kind, _ in found.values():
        kinds[kind] = kinds.get(kind, 0) + 1
    print(f"{len(found)} code addresses reached through pointers, not function starts: {kinds}")


if __name__ == "__main__":
    main()
