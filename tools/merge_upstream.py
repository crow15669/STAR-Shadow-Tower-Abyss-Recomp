"""Three-way merge of upstream (God Hand: Rearmed) changes into STAR's copy of the shared files.

    merge_upstream.py <base commit> <their commit> <path prefix or file> [...]   [--write]

For every file under the given paths that changed upstream between the two commits: base = upstream's at <base>,
theirs = upstream's at <their>, ours = STAR's file. Line ends are set aside for the merge and put back as STAR's file
has them. Without --write only reports (clean / conflicts / new / deleted)."""
import subprocess
import sys
import tempfile
from pathlib import Path

STAR = Path(r"D:/VS Code projs/STA Recomp")
GHR = STAR / "upstream" / "God-Hand-Rearmed"

def git(*args, cwd=GHR):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True)

def main(argv):
    write = "--write" in argv
    argv = [a for a in argv if a != "--write"]
    base, theirs, paths = argv[0], argv[1], argv[2:]
    names = git("diff", "--name-status", base, theirs, "--", *paths).stdout.decode().splitlines()
    result = {"clean": [], "conflict": [], "new": [], "deleted": [], "same": []}
    for line in names:
        status, name = line.split("\t", 1)
        ours_path = STAR / name
        theirs_bytes = git("show", f"{theirs}:{name}").stdout if status != "D" else None
        if status == "A" or not ours_path.exists():
            if theirs_bytes is None:
                continue
            result["new"].append(name)
            if write:
                ours_path.parent.mkdir(parents=True, exist_ok=True)
                ours_path.write_bytes(theirs_bytes)
            continue
        if status == "D":
            result["deleted"].append(name)
            continue
        base_bytes = git("show", f"{base}:{name}").stdout
        ours_bytes = ours_path.read_bytes()
        if b"\0" in ours_bytes[:8192] or b"\0" in theirs_bytes[:8192]:
            result["conflict"].append(name + " (binary)")
            continue
        crlf = ours_bytes.count(b"\r\n") > ours_bytes.count(b"\n") // 2
        strip = lambda b: b.replace(b"\r\n", b"\n")
        with tempfile.TemporaryDirectory() as tmp:
            a, o, b = Path(tmp) / "ours", Path(tmp) / "base", Path(tmp) / "theirs"
            a.write_bytes(strip(ours_bytes)); o.write_bytes(strip(base_bytes)); b.write_bytes(strip(theirs_bytes))
            merged = subprocess.run(["git", "merge-file", "-p", "-L", "STAR", "-L", "base", "-L", "upstream", str(a), str(o), str(b)],
                                    capture_output=True)
        text = merged.stdout
        if strip(ours_bytes) == text:
            result["same"].append(name)
            continue
        (result["conflict"] if merged.returncode != 0 else result["clean"]).append(name + (f" ({merged.returncode} conflicts)" if merged.returncode else ""))
        if write:
            ours_path.write_bytes(text.replace(b"\n", b"\r\n") if crlf else text)
    for kind, files in result.items():
        print(f"{kind}: {len(files)}")
        for f in files:
            print("   ", f)

if __name__ == "__main__":
    main(sys.argv[1:])
