"""Makes the release folder and zip from a finished build.

    python -I tools/package.py [--name STAR-windows-x64]

Output: build/release/<name>/ and build/release/<name>.zip. In it: the program (STAR.exe and its DLLs, resources),
the settings file at its defaults, the README, the licence and the third-party licences. Nothing of the game, no
logs, saves, caches or debug files. (The program writes its own log: no start script is needed.)

Run it in a clean checkout: it refuses a settings file that holds somebody's own disc image, preset or controls.
"""
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "bin"
PROGRAM = ["STAR.exe", "pcsx2gs.dll", "SDL3.dll", "shaderc_shared.dll"]

def fail(text):
    print(text)
    sys.exit(1)


def settings_text():
    """The settings file as it ships: the repository's, which must hold nobody's own choices."""
    text = (ROOT / "STAR settings.ini").read_text(encoding="utf-8").replace("\r\n", "\n")
    for line in text.splitlines():
        name, _, value = line.partition("=")
        if name.strip() in ("disc_image", "preset", "custom") and value.strip():
            fail(f'"STAR settings.ini" holds somebody\'s own "{name.strip()}": package from a clean checkout '
                 "(or stage the defaults with tools/stage_settings.sh and check the file out).")
    return text.replace("\n", "\r\n")  # Notepad-friendly


def main(args):
    name = args[args.index("--name") + 1] if "--name" in args else "STAR-windows-x64"
    for file in PROGRAM:
        if not (BIN / file).exists():
            fail(f"{BIN / file} is missing: build first (python -I tools/build.py build).")
    if not (BIN / "resources").is_dir():
        fail(f"{BIN / 'resources'} is missing.")
    release = ROOT / "build" / "release"
    out = release / name
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for file in PROGRAM:
        shutil.copy2(BIN / file, out / file)
    shutil.copytree(BIN / "resources", out / "resources")
    (out / "STAR settings.ini").write_bytes(settings_text().encode("utf-8"))
    shutil.copy2(ROOT / "README.md", out / "README.md")
    shutil.copy2(ROOT / "LICENSE", out / "LICENSE.txt")
    shutil.copytree(ROOT / "docs" / "licenses", out / "licenses")
    shutil.copy2(ROOT / "runtime" / "LICENSE", out / "licenses" / "PS2Recomp-GPL-3.0.md")
    shutil.copy2(ROOT / "thirdparty" / "pcsx2" / "COPYING.GPLv3", out / "licenses" / "PCSX2-GPL-3.0.txt")
    archive = release / (name + ".zip")
    archive.unlink(missing_ok=True)
    total = 0
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(out.rglob("*")):
            if path.is_file():
                z.write(path, Path(name) / path.relative_to(out))
                total += path.stat().st_size
    files = sum(1 for p in out.rglob("*") if p.is_file())
    print(f"{out}: {files} files, {total / 1e6:.1f} MB")
    print(f"{archive}: {archive.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main(sys.argv[1:])
