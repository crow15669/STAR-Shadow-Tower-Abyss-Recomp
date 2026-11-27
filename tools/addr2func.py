"""Which function holds a PS2 address: python -I tools/addr2func.py 0x128e84 [0x... ...]

Reads recomp/sta_functions.csv (Ghidra's export: real functions, plus entry_ labels inside them). Prints the real
function containing the address (FUN_ or a library name) and the nearest label at or before it.
"""
import bisect
import csv
import sys
from pathlib import Path

CSV = Path(__file__).resolve().parent.parent / "recomp" / "sta_functions.csv"


def load():
    funcs, labels = [], []
    with open(CSV, newline="") as f:
        for row in csv.DictReader(f):
            start, end = int(row["Start"], 16), int(row["End"], 16)
            (labels if row["Name"].startswith("entry_") else funcs).append((start, end, row["Name"]))
    funcs.sort()
    labels.sort()
    return funcs, labels


def main():
    funcs, labels = load()
    fstarts = [f[0] for f in funcs]
    lstarts = [l[0] for l in labels]
    for arg in sys.argv[1:]:
        addr = int(arg, 16)
        i = bisect.bisect_right(fstarts, addr) - 1
        func = funcs[i] if i >= 0 and funcs[i][0] <= addr < funcs[i][1] else None
        j = bisect.bisect_right(lstarts, addr) - 1
        label = labels[j] if j >= 0 else None
        text = f"0x{addr:08x}: "
        text += f"in {func[2]} (0x{func[0]:x}-0x{func[1]:x}, +0x{addr - func[0]:x})" if func else "in no Ghidra function"
        if label and (not func or label[0] >= func[0]):
            text += f", after label {label[2]}"
        print(text)


if __name__ == "__main__":
    main()
