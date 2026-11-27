# Building STAR from source

Windows only for now.

## What you need

- **Visual Studio 2022 Build Tools** (or Visual Studio 2022) with the "Desktop development with C++" workload,
  which brings MSVC, CMake and Ninja.
- **Python 3.**
- **Git** and an internet connection: the first configure downloads the libraries listed in
  `docs/licenses/README.md`.
- **Your own disc image** of Shadow Tower Abyss (SLPS-25217) with the 2011 English fan translation patch applied.
  The repository holds no game code or data: the game's program is recompiled from your disc.
- About 10 GB of free disk space.

## Steps

Run these from the repository's folder.

1. Copy the game's program off your disc image into `game\` (a folder git ignores):

   ```
   mkdir game
   python -I tools\iso_extract.py "<path to your disc image>.iso" SLPS_252.17 game\SLPS_252.17
   ```

2. Build the recompiler, recompile the game, compile its VU1 microcode, then build STAR:

   ```
   python -I tools\build.py recomp
   python -I tools\build.py generate
   python -I tools\build.py vu
   python -I tools\build.py build
   ```

   `generate` writes about 19,500 C++ files into `generated\`; the last step compiles them all, which takes the
   longest (about ten minutes on a 24-thread PC).

3. Finish the game's graphics microcode. The first build can only compile the part of it that is found in the
   program file; the rest is assembled by the game while it runs. Let the game run once, hidden, so that it writes
   the whole image out, then compile that and build again (quick this time):

   ```
   python -I tools\build.py boot --iso "<path to your disc image>.iso" --seconds 40
   python -I tools\build.py vu
   python -I tools\build.py build
   ```

   Skipping this leaves a third of the microcode interpreted: the game works but is slower, and its log says
   `[vu1-aot] ... the rest interpreted`.

4. The program is in `build\bin`: `STAR.exe`, `pcsx2gs.dll`, `SDL3.dll`, `shaderc_shared.dll` and `resources\`.
   Copy `STAR settings.ini` next to it, or let the launcher create one. `Start-STAR.cmd` in the repository's
   folder starts it with a log.

`boot` is also the way to check any build without a window: it runs hidden for the given time and prints the end
of its log. Exit code 124 means it was still running when the time was up, which is the good outcome.

## Making a release

`python -I tools\package.py` puts the program, the default settings file, a start script, the README and every
licence into `build\release\STAR-windows-x64` and zips it. Run it in a clean checkout: it refuses a settings file
that holds somebody's own disc image, preset or controls.

## After changing code

`python -I tools\build.py build` again. Two things to know before you start, both explained in
`docs/TECHNICAL_NOTES.md`:

- Do not edit a header that the generated code includes (`ps2_runtime.h`, `ps2_memory.h`, `ps2_vu1.h`,
  `ps2_vu1_hooks.h`), or all 19,500 files compile again.
- The program you recompile must be the exact one STAR is made for. Its SHA-256 is at the top of
  `docs/TECHNICAL_NOTES.md`; every hook address belongs to that file.
