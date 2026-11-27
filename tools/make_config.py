"""Builds the recompiler inputs from the reviewed lists:

    python -I tools/make_config.py

  recomp/sta_functions_ghidra.csv  Ghidra's export (ExportPS2Functions.java), kept as it came
  recomp/sta_sce_names.csv         Sony SDK names found by tools/sce_scan.py (Aura-Decomp-Tool's database)
  recomp/sta_sdk_names.csv         names matched against God Hand (tools/sdk_match.py)
  recomp/sta_hle_bindings.csv      the SDK functions the runtime handles (reviewed by hand, one reason per line)
->
  recomp/sta_functions.csv         Ghidra's functions, renamed where an SDK name is unambiguous
  recomp/sta.toml                  the recompiler config: GHR's settings, stubs = the bindings

Binding policy (God Hand: Rearmed's src/kamite/hle_bindings.cpp): only the SIF/IOP, file, debug-output and libcdvd
functions go to runtime handlers; everything else in the SDK runs as the game's own recompiled code.
"""
import csv
from collections import defaultdict
from pathlib import Path

RECOMP = Path(__file__).resolve().parent.parent / "recomp"
# Libraries Shadow Tower Abyss does not use: their names on tiny generic bodies are coincidences.
UNUSED_LIBRARIES = {"libhttps", "libeenet", "eenetctl", "libhig", "libccc", "libdbc"}


def read_csv(name):
    with open(RECOMP / name, newline="") as f:
        return list(csv.DictReader(f))


LOOSE_ENTRIES = []


def merge_loose_fragments(rows):
    """Code Ghidra never made a function (reached only through pointers: virtual methods, callbacks) is in its export
    as loose "entry_" label fragments, one per branch target. The recompiler registers no resume points for records
    named entry_ (isEntryFunctionName), so a thread that was switched out below such code could not come back to it
    (the crash after the new-game movie, 2026-11-22: return address 0x14dc90). Here every run of loose fragments
    becomes real functions named fn_<address>: a new one starts at the run's first fragment, at a pointer target
    (sta_entry_points.csv), at a jal target, or at a stack prologue that follows a return."""
    import bisect
    import struct
    elf = (RECOMP.parent / "game" / "SLPS_252.17").read_bytes()
    phoff, = struct.unpack_from("<I", elf, 28)
    _, off, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", elf, phoff)

    def word(a):
        return struct.unpack_from("<I", elf, off + a - vaddr)[0] if vaddr <= a < vaddr + filesz - 3 else 0

    named = sorted((int(r["Start"], 16), int(r["End"], 16)) for r in rows if not r["Name"].startswith("entry_"))
    named_starts = [s for s, _ in named]

    def in_named(a):
        i = bisect.bisect_right(named_starts, a) - 1
        return i >= 0 and a < named[i][1]

    code_end = max(int(r["End"], 16) for r in rows)
    jal_targets = set()
    for a in range(vaddr, code_end, 4):
        w = word(a)
        if w >> 26 == 3:
            jal_targets.add((a & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
    pointer_targets = set()
    if (RECOMP / "sta_entry_points.csv").exists():
        pointer_targets = {int(r["Address"], 16) for r in read_csv("sta_entry_points.csv")}

    def prologue_after_return(a):
        if (word(a) & 0xFFFF8000) != 0x27BD8000:      # addiu sp, sp, -N
            return False
        b = a - 4
        while b > vaddr and word(b) == 0 and a - b < 64:
            b -= 4
        return any(word(x) == 0x03E00008 or word(x) >> 26 == 2 for x in (b, b - 4))

    loose = sorted((r for r in rows if r["Name"].startswith("entry_") and not in_named(int(r["Start"], 16))),
                   key=lambda r: int(r["Start"], 16))
    keep = [r for r in rows if not (r["Name"].startswith("entry_") and not in_named(int(r["Start"], 16)))]
    made = []
    current = None
    for r in loose:
        s, e = int(r["Start"], 16), int(r["End"], 16)
        new = (current is None or s > current[1] or s in pointer_targets or s in jal_targets or prologue_after_return(s))
        if new:
            current = [s, e]
            made.append(current)
        else:
            current[1] = max(current[1], e)
    made_starts = {s for s, _ in made}
    # Every fragment start that is now inside a merged function stays reachable (the recompiler makes it an extra
    # entry of that function): some are targets of pointers no scan found (0x190770, a callback, 2026-11-22).
    global LOOSE_ENTRIES
    LOOSE_ENTRIES = sorted(int(r["Start"], 16) for r in loose if int(r["Start"], 16) not in made_starts)
    for s, e in made:
        keep.append({"Name": f"fn_{s:08x}", "Start": f"0x{s:08X}", "End": f"0x{e:08X}", "Size": str(e - s)})
    keep.sort(key=lambda r: (int(r["Start"], 16), r["Name"].startswith("entry_")))
    print(f"{len(loose)} loose label fragments merged into {len(made)} functions (fn_...)")
    return keep


# Past this address the "code" range holds data (kernel patch blobs, tables): gaps there are not code.
GAP_CODE_LIMIT = 0x001EDE00


def close_gaps(rows, bound_addresses):
    """Code between two functions that is in neither (Ghidra's function bodies leave out blocks it found no path to:
    cases of a switch it could not resolve, small routines nothing calls directly) had no record at all. The recompiler
    still reached some of it, but a return address there was not a resume point (the crash opening the map screen,
    2026-11-22: 0x198c24, in 0x198bc4-0x198d80 after FUN_00198570). Every such gap now belongs to a function: the one
    before it is extended over it, or, after a runtime-bound SDK function (whose body is replaced), it becomes fn_<address>."""
    import struct
    elf = (RECOMP.parent / "game" / "SLPS_252.17").read_bytes()
    phoff, = struct.unpack_from("<I", elf, 28)
    _, off, vaddr, _, _, _, _, _ = struct.unpack_from("<8I", elf, phoff)

    def word(a):
        return struct.unpack_from("<I", elf, off + a - vaddr)[0]

    funcs = sorted((r for r in rows if not r["Name"].startswith("entry_")), key=lambda r: int(r["Start"], 16))
    extended = created = 0
    new_rows = []
    for a, b in zip(funcs, funcs[1:]):
        end, nxt = int(a["End"], 16), int(b["Start"], 16)
        if nxt <= end or end >= GAP_CODE_LIMIT:
            continue
        first = next((x for x in range(end, nxt, 4) if word(x) != 0), None)
        if first is None:
            continue                      # nop padding only
        if int(a["Start"], 16) in bound_addresses:
            new_rows.append({"Name": f"fn_{first:08x}", "Start": f"0x{first:08X}", "End": f"0x{nxt:08X}", "Size": str(nxt - first)})
            created += 1
        else:
            a["End"], a["Size"] = f"0x{nxt:08X}", str(nxt - int(a["Start"], 16))
            extended += 1
    rows = rows + new_rows
    rows.sort(key=lambda r: (int(r["Start"], 16), r["Name"].startswith("entry_")))
    print(f"gaps with code: {extended} functions extended, {created} new fn_ functions")
    return rows


def main():
    bindings = read_csv("sta_hle_bindings.csv")
    bound = {int(b["Address"], 16): b for b in bindings}

    # Candidate names per address: Aura's (minus unused libraries), then God Hand's.
    names_at = defaultdict(set)
    addresses_of = defaultdict(set)
    for r in read_csv("sta_sce_names.csv"):
        if r["Library"] in UNUSED_LIBRARIES:
            continue
        a = int(r["Address"], 16)
        names_at[a].add(r["Name"])
        addresses_of[r["Name"]].add(a)
    for r in read_csv("sta_sdk_names.csv"):
        a = int(r["Address"], 16)
        if a not in names_at:
            names_at[a].add(r["Name"])
            addresses_of[r["Name"]].add(a)
    chosen = {}
    for a, names in names_at.items():
        if len(names) == 1:
            (name,) = names
            if len(addresses_of[name]) == 1:
                chosen[a] = name
    for a, b in bound.items():  # the reviewed bindings decide their own addresses
        chosen[a] = b["Name"]

    rows = read_csv("sta_functions_ghidra.csv")
    # A bound function Ghidra merged into the one before it (small functions in a row, no call between them): split the
    # containing function at the bound address.
    starts = {int(r["Start"], 16) for r in rows}
    split = 0
    for a, b in sorted(bound.items()):
        if a in starts:
            continue
        for r in rows:
            s, e = int(r["Start"], 16), int(r["End"], 16)
            if s < a < e and not r["Name"].startswith("entry_"):
                r["End"], r["Size"] = f"0x{a:08X}", str(a - s)
                rows.append({"Name": b["Name"], "Start": f"0x{a:08X}", "End": f"0x{e:08X}", "Size": str(e - a)})
                starts.add(a)
                split += 1
                break
        else:
            # Not inside any function: a gap Ghidra never disassembled (nothing calls it directly). The function runs
            # to the next known start.
            e = min(x for x in starts if x > a)
            rows.append({"Name": b["Name"], "Start": f"0x{a:08X}", "End": f"0x{e:08X}", "Size": str(e - a)})
            starts.add(a)
            split += 1
    rows.sort(key=lambda r: (int(r["Start"], 16), r["Name"].startswith("entry_")))
    if split:
        print(f"{split} functions split at bound addresses")
    renamed = 0
    used = set()
    for r in rows:
        a = int(r["Start"], 16)
        if a in chosen and chosen[a] not in used:
            r["Name"] = chosen[a]
            used.add(chosen[a])
            renamed += 1
    missing = [b for a, b in bound.items() if a not in starts]
    if missing:
        raise SystemExit("bindings without a function start in Ghidra's list: " +
                         ", ".join(f"{b['Name']}@{b['Address']}" for b in missing))
    rows = merge_loose_fragments(rows)
    rows = close_gaps(rows, set(bound))
    with open(RECOMP / "sta_functions.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["Name", "Start", "End", "Size"])
        w.writeheader()
        w.writerows(rows)

    entry_points = []
    entry_csv = RECOMP / "sta_entry_points.csv"
    if entry_csv.exists():
        entry_points = [r["Address"] for r in read_csv("sta_entry_points.csv")]
    known = {int(a, 16) for a in entry_points}
    entry_points += [f"0x{a:08X}" for a in LOOSE_ENTRIES if a not in known]

    stubs = []
    for b in bindings:
        handler = b["Handler"] or b["Name"]
        stubs.append(f'    "{handler}@{b["Address"]}", # {b["Name"]}: {b["Why"]}')
    toml = f"""# Shadow Tower Abyss (SLPS-25217, English-patched): recompiler config. GENERATED by tools/make_config.py from
# sta_hle_bindings.csv (edit that file, then run make_config.py again). Paths are relative to this folder; tools/build.py
# generate rewrites input and output.
[general]
input = "../game/SLPS_252.17"
output = "../generated"
ghidra_output = "sta_functions.csv"
single_file_output = false
output_worker_threads = 8
# As GHR (recomp/kamite.toml): syscalls run through the runtime's syscall dispatch, no COP0/CACHE patches.
patch_syscalls = false
patch_cop0 = false
patch_cache = false
# SDK functions reach runtime handlers only through the stubs below (address-pinned), never by name.
auto_stub_by_name = false
# Tail jumps go through the dispatch table, so a replaced function is reached by every caller (as GHR).
direct_tail_calls = false
# The SDK functions the runtime handles ({len(stubs)}; sta_hle_bindings.csv says why each one is here).
stubs = [
{chr(10).join(stubs)}
]
skip = []
# Code addresses reached through pointers that are not function starts ({len(entry_points)}; tools/find_code_pointers.py ->
# sta_entry_points.csv): inside a function they become extra entries, elsewhere small standalone functions.
entry_points = [
{chr(10).join('    "' + a + '",' for a in entry_points)}
]
"""
    (RECOMP / "sta.toml").write_text(toml, encoding="utf-8", newline="\n")
    print(f"{renamed} functions named, {len(stubs)} bindings -> sta_functions.csv, sta.toml")


if __name__ == "__main__":
    main()
