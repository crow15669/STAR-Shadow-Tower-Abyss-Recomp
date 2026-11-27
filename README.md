# STAR: Shadow Tower Abyss Recomp

A native Windows version of **Shadow Tower Abyss** (FromSoftware, PlayStation 2, 2003), made by static
recompilation: the game's own PS2 program is translated into a PC program once, instead of being emulated while
you play.

STAR contains **no game data**. You need your own disc image of the game. It has only been tested with the
English-patched version of SLPS-25217 (the Japanese release), and it accepts no other version.

This is an unofficial fan project made with AI assistance. It is not affiliated with or endorsed by FromSoftware
or Sony.

## What you get

- **Sharper picture:** up to 6x the PS2's resolution, FXAA, and the game's flicker filter and interlace blur removed.
- **Unlocked framerate:** in-between pictures for the camera and everything that moves, up to your screen's
  refresh rate. The game's logic still runs at its own 30 frames a second.
- **Reduced input lag:** about 45 ms less delay between pressing a button and seeing it happen.
- **True analog aiming** with the right stick.
- **Mouse and keyboard play** with rebindable controls and low-delay mouse look (experimental: the game was never
  designed for it).
- **Shadow fixes:** sharp shadow edges, shadows that no longer vanish up close or pass through walls, and an option
  to hide the game's buggy self-shadows.
- **Text fixes** for two layout faults of the English patch. Its wording is untouched.
- **A launcher** with three presets, and an in-game menu (F2) that changes every setting while you play.
- **Save states** (F5 saves, F9 loads) next to the game's own memory card saves.

Presets:

| Preset | What it is |
|---|---|
| Original | As it was on the PlayStation 2 |
| Remastered | The original, sharper and smoother, with reduced input lag and analog aiming |
| STAR | Remastered, plus the shadow fixes and mouse and keyboard play |

## What you need

- Windows 10 or 11, 64-bit.
- A graphics card with Vulkan support.
- A disc image (ISO or CHD) of the Japanese release, **SLPS-25217**, with the **2011 English fan translation patch**
  applied. STAR checks the disc's program when you choose it and refuses any other version.

STAR has so far been tested on one PC. Reports from other setups are welcome.

## Playing

1. Download the latest release from the
   [releases page](https://github.com/crow15669/STAR-Shadow-Tower-Abyss-Recomp/releases) and unzip it anywhere.
2. Start `STAR.exe`.
3. In the launcher, choose your disc image, pick a preset and press **Play**.

Your choices are kept in `STAR settings.ini` next to the program.

| Key | What it does |
|---|---|
| F2 | The in-game menu: Graphics, Gameplay, Experimental |
| F5 / F9 | Save state / load state |
| F11 | Full screen |
| F3 | Framerate display |
| F1 | Save the last few seconds of pictures for a bug report (when switched on under Experimental) |

A controller works as it does on the PS2. For mouse and keyboard, the controls are listed, and can be changed,
under Experimental > Change controls.

Memory card saves are in the `memcards` folder and save states in `states`, both next to the program.

## Known issues

- The first start, and the first time each visual effect appears, can stall for a moment while shaders are
  prepared. Later runs are smooth.
- Menus and movies stay at the game's own framerate; only 3D scenes get in-between pictures.
- On a second monitor with a different refresh rate than the main one, motion can be uneven.
- The game's own creature self-shadows are faulty. The STAR preset hides them.

More, with the reasons, in [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md) (sections 6 and 7).

## Reporting a bug

STAR writes a log of every run to `logs\play_last.log` next to the program, and keeps the three runs before it.
Open an issue with that file and a description of what happened and where. The log records your graphics card and
its driver, your screen's resolution and refresh rate and your STAR settings, which is what finding a bug needs. It
holds no folder names from your PC.

For something that flickers once and will not repeat, switch on "Keep the last few seconds" under Experimental and
press F1 right after it happens; the pictures are saved in `logs\pictures_<number>`, with the settings as they were
at that moment.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md). If you want to change or fork STAR,
[docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md) describes how the game and the port work.

## Credits

STAR stands on other people's work:

- **Shadow Tower Abyss** by FromSoftware, and the authors of its 2011 English fan translation.
- **[PS2Recomp](https://github.com/ran-j/PS2Recomp)**, the static recompiler and PS2 runtime, and the forks named in
  [runtime/README.md](runtime/README.md).
- **[God Hand: Rearmed](https://github.com/God-Hand-Rearmed/God-Hand-Rearmed)**, whose improved copy of that
  runtime and whose build of PCSX2's renderer STAR started from.
- **[PCSX2](https://github.com/PCSX2/pcsx2)**, whose GS renderer draws the picture.
- The movie decoder from the Ace Combat 5 static recompilation, and the libraries listed in
  [docs/licenses](docs/licenses/README.md).

## Licence

STAR's code is under the GNU General Public License, version 3 ([LICENSE](LICENSE)), like PS2Recomp and PCSX2.
The complete source code of every release is at
<https://github.com/crow15669/STAR-Shadow-Tower-Abyss-Recomp>.
Third-party licences are in [docs/licenses](docs/licenses/README.md).
