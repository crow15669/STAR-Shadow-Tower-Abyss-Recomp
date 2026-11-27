"""STAR build steps (Windows, Visual Studio 2022 Build Tools; run with python -I from anywhere).

    python -I tools/build.py recomp                  build the recompiler (runtime/ps2xRecomp) into build/
    python -I tools/build.py generate                recompile game/SLPS_252.17 (recomp/sta.toml) into generated/
    python -I tools/build.py configure [-DOPT=...]   CMake configure (Ninja, RelWithDebInfo) into build/
    python -I tools/build.py build                   compile STAR.exe (build/bin)
    python -I tools/build.py boot [--seconds N]      a hidden, time-capped run; log in build/bin/logs/

The disc image: --iso <path>, else STAR_ISO, else the path written in game/iso_path.txt (game/ is never committed).
Modelled on God Hand: Rearmed's build.py.
"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
BIN = BUILD / "bin"
GENERATED = ROOT / "generated"
STAGING = BUILD / "generated_staging"
TOML = ROOT / "recomp" / "sta.toml"
VSENV = ROOT / "tools" / "win" / "vsenv.cmd"
RECOMP = BUILD / "runtime" / "ps2xRecomp" / "ps2_recomp.exe"
JOBS = os.environ.get("STAR_JOBS", str(os.cpu_count() or 8))


def find_vsroot():
    if os.environ.get("VSROOT"):
        return os.environ["VSROOT"]
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        out = subprocess.run([str(vswhere), "-version", "[17.0,18.0)", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip().splitlines()
        if out:
            return out[0]
    return None


def in_vs(*args):
    """Runs a command inside the Visual Studio x64 environment; returns its exit code."""
    env = dict(os.environ)
    vsroot = find_vsroot()
    if not vsroot:
        print("Visual Studio 2022 with C++ not found (set VSROOT).")
        return 1
    env["VSROOT"] = vsroot
    # cmd /s /c "<line>": cmd strips exactly the outer quotes, so paths with spaces survive (as GHR's build.py).
    line = 'cmd /s /c "' + subprocess.list2cmdline([str(VSENV), *[str(a) for a in args]]) + '"'
    print("+", line, flush=True)
    return subprocess.run(line, env=env).returncode


def find_iso(args):
    if "--iso" in args:
        return Path(args[args.index("--iso") + 1])
    if os.environ.get("STAR_ISO"):
        return Path(os.environ["STAR_ISO"])
    stored = ROOT / "game" / "iso_path.txt"
    if stored.exists():
        return Path(stored.read_text(encoding="utf-8").strip())
    return None


def configure(extra=()):
    args = ["cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", *extra]
    return in_vs(*args)


def cmd_recomp(args):
    if configure(["-DSTAR_BUILD_RECOMP=ON"]):
        return 1
    return in_vs("cmake", "--build", BUILD, "--target", "ps2_recomp", "--", "-j", JOBS)


def sync_tree(src, dst):
    """Copies changed files from src into dst and deletes files dst has that src has not (keeps unchanged files'
    timestamps, so an incremental build recompiles only what the recompiler changed)."""
    dst.mkdir(parents=True, exist_ok=True)
    src_names = {p.name for p in src.iterdir() if p.is_file()}
    changed = removed = 0
    for name in src_names:
        a, b = src / name, dst / name
        if not b.exists() or a.stat().st_size != b.stat().st_size or a.read_bytes() != b.read_bytes():
            shutil.copy2(a, b)
            changed += 1
    for p in dst.iterdir():
        if p.is_file() and p.name not in src_names:
            p.unlink()
            removed += 1
    print(f"generated/: {changed} files written, {removed} removed, {len(src_names) - changed} unchanged")


def cmd_generate(args):
    if not RECOMP.exists() and cmd_recomp(args):
        return 1
    shutil.rmtree(STAGING, ignore_errors=True)
    STAGING.mkdir(parents=True)
    text = TOML.read_text(encoding="utf-8")
    lines = []
    for line in text.splitlines():
        key = line.split("=")[0].strip()
        if key == "input":
            line = f'input = "{(ROOT / "game" / "SLPS_252.17").as_posix()}"'
        elif key == "output":
            line = f'output = "{STAGING.as_posix()}"'
        elif key == "ghidra_output":
            line = f'ghidra_output = "{(TOML.parent / "sta_functions.csv").as_posix()}"'
        lines.append(line)
    staged = BUILD / "sta.staging.toml"
    staged.write_text("\n".join(lines) + "\n", encoding="utf-8")
    log = BUILD / "recomp.log"
    env = dict(os.environ, PS2X_CODEGEN="classic")
    print(f"+ {RECOMP} {staged}  (output in {log})", flush=True)
    with open(log, "w", encoding="utf-8") as out:
        code = subprocess.run([str(RECOMP), str(staged)], cwd=BUILD, stdout=out, stderr=subprocess.STDOUT, env=env).returncode
    report = log.read_text(encoding="utf-8", errors="replace")
    if "========== PS2Recomp report" in report:
        print(report[report.index("========== PS2Recomp report"):].split("Events:")[0].rstrip())
    if code or not (STAGING / "register_functions.cpp").exists():
        print(f"ps2_recomp failed ({code}); see {log}")
        return code or 1
    sync_tree(STAGING, GENERATED)
    # configure again: a first configure, made before there was any recompiled code, has no STAR.exe target
    return configure()


def cmd_configure(args):
    return configure(args)


def cmd_build(args):
    if not (BUILD / "build.ninja").exists() and configure():
        return 1
    return in_vs("cmake", "--build", BUILD, "--target", "star", "--", "-j", JOBS)


def cmd_boot(args):
    exe = BIN / "STAR.exe"
    if not exe.exists():
        print(f"{exe} not built yet")
        return 1
    iso = find_iso(args)
    if not iso:
        print("No disc image: --iso <path>, STAR_ISO, or game/iso_path.txt")
        return 2
    seconds = args[args.index("--seconds") + 1] if "--seconds" in args else "30"
    logs = BIN / "logs"
    logs.mkdir(exist_ok=True)
    log = logs / f"boot_{time.strftime('%Y%m%d-%H%M%S')}.log"
    print(f"+ STAR.exe --hidden --seconds {seconds}  (log: {log})", flush=True)
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        code = subprocess.run([str(exe), "--iso", str(iso), "--hidden", "--seconds", seconds], cwd=BIN,
                              stdout=out, stderr=subprocess.STDOUT).returncode
    shutil.copy2(log, logs / "boot_last.log")
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    print("\n".join(lines[-40:]))
    print(f"exit code {code}" + (" (time limit: still running)" if code == 124 else ""))
    return code


def cmd_vu(args):
    """VU1 microcode ahead of time: build/vu1_images (uploads found in the program by tools/vu/extract_uploads.py, plus
    the vu1_*.bin / vup_*_cap_*.bin files of build/vu1_capture and of any folder given here: captures of a run with
    PS2X_VU1_CAPTURE=<folder>) -> vu1_recomp -> generated/vu1. Then 'build' again."""
    images = BUILD / "vu1_images"
    images.mkdir(parents=True, exist_ok=True)
    code = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "vu" / "extract_uploads.py"), str(images),
                           str(ROOT / "game" / "SLPS_252.17")]).returncode
    if code:
        return code
    copied = 0
    # (bin/logs/vu1_missing: where a run writes the microcode it had to interpret; a first build from scratch gets
    # the game's whole image that way: build, boot, vu, build)
    for folder in [BUILD / "vu1_capture", BIN / "logs" / "vu1_missing", *[Path(a) for a in args]]:
        if not folder.is_dir():
            continue
        for image in sorted(folder.glob("vu1_*.bin")) + sorted(folder.glob("vup_*_cap_*.bin")):
            size = image.stat().st_size
            if (size == 16384 or image.name.startswith("vup_")) and size % 8 == 0 and not (images / image.name).exists():
                shutil.copy2(image, images / image.name)
                copied += 1
    print(f"captured files: {copied} new")
    if configure(["-DSTAR_BUILD_TOOLS=ON"]) or in_vs("cmake", "--build", BUILD, "--target", "vu1_recomp", "--", "-j", JOBS):
        return 1
    staging = BUILD / "vu1_staging"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")   # vu1_recomp links the runtime, which loads SDL3.dll
    tool = BUILD / "runtime" / "ps2xTest" / "vu1_recomp.exe"
    code = subprocess.run([str(tool), str(images), str(staging)], env=env).returncode
    if code:
        print(f"vu1_recomp failed ({code}); generated/vu1 is left as it was")
        return code
    sync_tree(staging, GENERATED / "vu1")
    return 0


COMMANDS = {"vu": cmd_vu, "recomp": cmd_recomp, "generate": cmd_generate, "configure": cmd_configure, "build": cmd_build,
            "boot": cmd_boot}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(1)
    sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]))
