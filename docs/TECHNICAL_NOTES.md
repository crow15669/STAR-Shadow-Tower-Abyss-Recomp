# STAR technical notes

For anyone who wants to change or fork STAR (Shadow Tower Abyss Recomp), with or without an AI assistant. This is
what was learned about the game and the port while building it: how the pieces fit, where things live in the game's
memory, what is known to be wrong, and what cannot be had from this design. It is a map, not a tutorial; the code
comments carry the details and name the functions mentioned here.

Everything below was found by reading the game's compiled code (Ghidra) and by measuring runs. There is no source
code of the game. Function names like `FUN_001c4760` are Ghidra's: the number is the address.

**Every address in this file is for one program only:** `SLPS_252.17` from the Japanese disc (SLPS-25217) with the
2011 English fan patch, 1,056,944 bytes, SHA-256
`3c2097cd15ae6957ce7dfd5c424a073156ac22eb9ef21a744dc2f34ded12c6d8`. STAR refuses any other program at start-up
(`checkDisc` in `src/main.cpp`). The unpatched Japanese program was never tried.

## Contents

1. What STAR is made of
2. Building, and the rules that save hours
3. Fixes made to the runtime for this game
4. How the game works (frame, display, input, player, items, maps, creatures, sound, effects, text, shadows)
5. STAR's features and how each is done
6. Known issues
7. Hard limits
8. Testing without a screen: switches, tools, method
9. Settings and defaults

---

## 1. What STAR is made of

STAR is a static recompilation: the PS2 program's MIPS code is turned into C++ once, compiled, and run on a runtime
that stands in for the PS2 (memory, kernel calls, the IOP sound processor, the GS graphics chip). No game data is in
the repository; everything is read from the player's own disc image.

| Folder | What it is |
|---|---|
| `src/` | STAR's own layer: start-up and disc check (`main.cpp`), the launcher, every game-specific fix and feature (`src/star/`) |
| `runtime/` | The PS2 runtime and recompiler, from God Hand: Rearmed's improved copy of PS2Recomp, with fixes marked `STAR` |
| `thirdparty/pcsx2`, `thirdparty/pcsx2-gs` | PCSX2's GS renderer built as `pcsx2gs.dll` (Vulkan), with STAR's additions |
| `recomp/` | Recompiler inputs: the function list, entry points, library names and bindings |
| `tools/` | Build script, analysis scripts, Ghidra scripts |
| `generated/` (not in git) | The recompiled game, about 19,500 functions; rebuilt from the disc |
| `game/` (not in git) | Files copied off the player's disc for the build |

The main files of `src/star/`:

| File | Job |
|---|---|
| `game_fixes.cpp` | Hooks on game functions: sound start-up, scanmask, analog aiming, mouse and keyboard play, late input, the "in view" test, text fixes |
| `frame_double.cpp` | Smooth motion: in-between pictures made from the game's own drawing stream |
| `live_look.h` | What the game thread tells the picture worker about the mouse's turn each frame |
| `shadow_volumes.cpp` | Creature shadows up close, and the data for "shadows stop at walls" |
| `bindings.cpp` | Rebindable mouse and keyboard controls |
| `options.cpp` | One table of every setting: label, note, choices, default, presets. The launcher and the F2 menu both draw from it |
| `settings_menu.cpp` | The in-game F2 pages (Graphics, Gameplay, Experimental) |
| `debug_menu.cpp` | The F2 Debug page: items, creatures, jumps between maps |
| `launcher.cpp` | The launcher window (ImGui on the game's own window) |

Disc contents, for orientation: `SLPS_252.17` (program), `IOPRP250.IMG` (IOP update), `TARGET\STAZ.STZ` (all game
data, 253 MB), `OPENING/PROLOGUE/ENDING.PSS` (movies), `STA_SR/STA_TI/STA_GO.ADX` (CRI ADX music). The program is
stripped and uses Sony's SDK 2.5 libraries. It holds no debug menu of its own: only file name formats
(`map/m%4.04dm.stm`, `chr/c%4.04dm.stm`, `item/i%4.04dm.stm`, `scr/chr00NN_.bin`, `se/m%03ds.mse`).

## 2. Building, and the rules that save hours

Windows, Visual Studio 2022 Build Tools (MSVC, CMake, Ninja), Python 3. All steps go through one script:

```
python -I tools/build.py recomp       build the recompiler
python -I tools/build.py generate     recompile game/SLPS_252.17 (recomp/sta.toml) into generated/
python -I tools/build.py vu           compile the game's VU1 microcode ahead of time into generated/vu1
python -I tools/build.py configure    CMake configure into build/
python -I tools/build.py build        compile STAR.exe and pcsx2gs.dll into build/bin
python -I tools/build.py boot         a hidden, time-capped test run
```

How the function list was made (only needed again if the recompile is redone from scratch): Ghidra with the
ghidra-emotionengine-reloaded add-on exports the functions; `tools/make_config.py` merges Ghidra's loose `entry_`
fragments into functions and closes gaps between functions (switch cases Ghidra left out), keeping every old
fragment start as an entry point; `tools/find_code_pointers.py` adds function-pointer targets Ghidra missed;
`tools/sce_scan.py` names Sony library functions by fingerprint so the runtime can bind them.

Rules learned the hard way:

- **Never edit a header that the generated code includes** (`ps2_runtime.h`, `ps2_memory.h`, `ps2_vu1.h`,
  `ps2_vu1_hooks.h`). All ~18,600 generated files rebuild. Even restoring the file with git gives it a new date and
  does the same. Put new runtime entry points in headers the generated code does not include.
- **A compiled program cannot take a PNACH-style code patch.** Every change to the game's behaviour is a hook: a
  replacement for one recompiled function that does something and then (usually) calls the original.
- **A resume address must be a registered entry point.** The recompiled code can only be re-entered at addresses
  the recompiler knows. Two early crashes were resumes inside code Ghidra had not attributed to a function. After
  changing the function list, check that every `jal`/`jalr` return address is a resume point
  (`tools/audit_resume_points.py`).
- **Calling a game function from the host must run it to its end.** Generated code returns early at the scheduler's
  safe points (every call, every backward branch) and expects to be resumed. A host caller that does not allow for
  that gets a half-finished answer. `callGame` in `debug_menu.cpp` wraps the call in
  `eeScheduler().beginHostCall()`; do the same for any new call.
- **VU1 microcode is compiled ahead of time.** The game uploads one program set (one 15,472-byte upload). Only
  about two thirds of it can be found in the program file; the whole image exists only while the game runs. A run
  that meets microcode it has no compiled code for writes the image to `logs/vu1_missing` and says so in its log
  (`[vu1-aot] ... the rest interpreted`); the `vu` step picks that file up. A first build from scratch therefore
  goes build, boot, vu, build (`docs/BUILDING.md`). With nothing compiled the game ran at about a third of full
  speed.
- **Line endings.** Several files are CRLF (`STAR settings.ini`, `pcsx2gs_api.h`, the runtime's input files, some
  shaders). `sed -i` in Git Bash silently turns them into LF and the whole file shows as changed.
- **The shader table.** A new `ShaderConvert` entry in the renderer must also be listed in `HasColorOutput`
  (`GSDevice.h`), or its pass writes no colour.

## 3. Fixes made to the runtime for this game

These were faults of the runtime that this game exposed. Any other game on the same runtime would meet them too.

| Symptom | Cause | Where |
|---|---|---|
| Stuck in `sceCdInit` at boot | SIF RPC functions of SDK 2.5 had no names to bind | `recomp/sta_hle_bindings.csv` |
| No sound server; weapons dead; music cues missing | The game's sound RPC server thread (priority 0x30) only runs on a PS2 while the main thread blocks in a synchronous RPC; the runtime's RPCs never block | `soundCommandHook` in `game_fixes.cpp` yields before command 0x20000 |
| Menu never appears after Start | Sony's MCMAN paces its memory card probe with `SetAlarm`, which was a stub | IOP alarms in `ps2xIOP/.../iop_emulator.cpp` |
| Movies freeze or break up | The toIPU DMA ignored the tag a chain restart writes into CHCR, and could be re-entered before its registers moved on | `ps2_memory.cpp` (toIPU) |
| Movies split in two halves, flicker | `GsSetCrt` did not set SMODE1 / SYNCV as the BIOS does, so the renderer misread the video mode | `Kernel/Syscalls/System.cpp` |
| A one-shot music cue loops for ever | The IOP stream module follows its voice with the SPU2 interrupt, which the SPU2 lacked | `iop_spu2.cpp` |
| Attacks stop working some time after loading a save state | SPU2 DMA progress was kept outside the saved state; after a load the last transfer read as still running, and the game takes no attack until its weapon sounds are uploaded | `resetSpu2DmaProgress()` in `IopEmulator::loadState` |
| Stutter with sound drop-outs the first time an effect appears | A shader compile stops the GS thread 30-100 ms and its queue of 64 chunks blocked the game | `gs_threaded_backend.cpp`: the queue may grow while its oldest chunk is under 150 ms old |
| Stutters when walking into a new area, disc image on a hard disk | The image was read with plain blocking reads while the game waits | `ps2_cd_image.cpp` reads the whole image through once in the background at start (`PS2X_CD_WARM=0` turns it off) |
| A field (16.7 ms) of delay on every 2D screen | The field was handed to the GS at the vblank, before the game's display swap | `PS2X_GS_VSYNC_AFTER_FLIP=1` (set in `main.cpp`) |
| "(Not Responding)" and a white tint over a running game | Windows flags the window although its loop runs | `window_watch.cpp` turns window ghosting off |

Two renderer options inherited from God Hand: Rearmed are switched off in `main.cpp` because this game uses a
16-bit depth buffer: `PS2X_PCSX2_PRECISE_DEPTH=0` and `PS2X_PCSX2_PERSPECTIVE_SHADING=0`.

## 4. How the game works

### The frame

Game logic runs at **30 frames a second**; the display at 60 fields. The play loop is `FUN_0012b350`, once per
frame:

`FUN_00155370` (wait for the vblank, swap) -> `FUN_0015fc10` (read the pad) -> `FUN_001296c0` -> `FUN_001552a0` (hand
the frame built **last** time to VIF1) -> `FUN_0012a520` (the logic, f12 = 1/30) -> `FUN_0012a070` (build the next
frame's drawing list).

Two consequences that shape everything else:

- A frame's picture is sent for drawing **one frame (33 ms) after** the input that made it was read.
- The game tests what is in view with the camera of the frame **before** the one it then draws with.

1/30 appears as an inline constant 14 times and 30.0 32 times in the code. There is no single frame-time variable,
so the logic cannot simply be run at 60.

The vblank handler `FUN_001e8cb0` shows each logic frame twice, once per field, into alternating buffers (pages 0
and 0x8c; depth at page 0x118, 16 bits, 640x448). In play the video mode is interlaced.

### Full-screen passes after the scene

The driver is `FUN_00159680`; the byte at 0x2f2078 says which passes run.

| Pass | Function | Note |
|---|---|---|
| Scanmask blend (the previous frame on every other line) | `FUN_00158bf0` sets GS SCANMSK through `FUN_001543e0(register, value)` | Blinking edges on a progressive screen. STAR turns SCANMSK 2/3 into 4, as PCSX2's "Remove Scanmask Blur" patch does |
| Turn shift (the picture copied onto itself, shifted, in 32-pixel strips while turning) | `FUN_00158bf0` | Made for interlaced TVs; smears and snaps on a PC. STAR skips it |
| Flicker filter (the picture blended one line down) | `FUN_00158900` | Setting `flicker_filter` |
| Glow (the picture at 256x256 added back four times) | `FUN_001580e0` | Setting `glow` |
| Fades | `FUN_00153b50`, `FUN_00153d80` | |

2D quads go through `FUN_00155bd0` / `FUN_00155ac0` -> `FUN_00155db0` -> VU1 program 0x3c30.

### The drawing stream (what smooth motion reads)

One frame is one VIF1 job of about 1-2.4 MB.

- **Camera:** UNPACK of 16 quad words to VU1 address 0 and of 4 to address 16 (the view matrix), three times a frame
  (main view, the shadow buffer's own projection, main again). The view matrix in game memory is at 0x2f16c0.
- **Objects:** UNPACK of 4 quad words to address 24 (the piece's matrix), more bones at 28 / 32 / 36 (the count is in
  `VU 40.w & 3`), then vertices relative to TOPS; VU1 program 0 sets up, 0x4b0 draws.
- **Sprites (particles):** a 0x30-byte record sent with VIF code `0x6c03c002`, drawn by the program at 0x4ee.
- **Shadow volumes:** program 0x678, see Shadows below.
- The game's packet builder: `FUN_00154a80(n)` gives the next n quad words of the packet being built;
  `FUN_00176ae0` commits it into a layer.
- Every model piece is drawn by `FUN_0016dc70` or `FUN_0016ded0` (a0 = piece, a1 = the model's draw object, a2 =
  its instance).

### What is "in view"

`FUN_00153fa0` (a0 = sphere x y z r in the world) answers: -1 outside (not drawn), 0 inside, 1 across the view's
edge but inside the GS guard band (0 and 1 go to a fast VU1 program that does **not** clip), 2 across the guard band
(the clipping program). Camera matrix at 0x2f1640; half angles 37.5 degrees sideways, 28.2 up and down; guard band
78.2 degrees; far plane 20000.

This matters for anything that draws a frame from another camera than the game's: things at the edge are missing,
and unclipped triangles that leave the GS's coordinate space wrap round into huge sheets across the screen.

### Input

- `FUN_001d7350` runs the pads (0x1c0 bytes a pad at 0x4b6900; libpad's data for pad 0 at 0x4b6a00).
- `FUN_0015f540` builds the game's own pad state at **0x317230**: buttons held +0, newly pressed +8, the four sticks
  as floats from +0x0c (square dead zone of 30 of 127), then 12 pressures as floats from +0x1c.
- `FUN_001c4760(player, amounts)` turns it into 14 "amounts" by the control type and runs only while the player may
  act. The control type is `FUN_001a9450()` = the byte at `(*0x201E10) + 7`, asked only by the player's own code.
  Types 4 and 5: left stick moves and strafes, right stick turns and looks; the d-pad walks and strafes; L1/R1/L2/R2
  are the four swings; R1 draws and fires a gun, L1 puts it away.
- Buttons in play (`FUN_001c0e60`): cross = menu, circle = act, triangle = switch weapons, square = magic, start =
  use the shortcut item, select = pause.

### The player

`inner = *(player + 0x518)`. Useful offsets from `inner`:

| Offset | What |
|---|---|
| +0 | The eye (three floats) |
| +0x198 | Pointer to the view angles: pitch at +0x20 (clamped to -60..+35 degrees), yaw at +0x24 |
| +0x1c0 | The player's place (three floats; **y grows downward**; about 220 above the feet) |
| +0x510 | Pointer to the stats (ailments) |
| +0x516 | Attack count (a swing needs it above 0; it returns about 15 frames after a swing) |
| +0x51c | Weapon kind: 7, 8, 10-18 guns; 9 and 0x13 bow-like; the rest melee |
| +0x52f | Frames since the area began (the player is held still for the first 45) |
| +0x540, +0x544 | The two equipped weapons |
| +0x54c | The swing direction the game remembers (the same one twice is refused) |
| +0x555 | Weapon state: 0 at rest or put away, 3 gun out or bow drawn, 4 firing or swinging, 8 after a swing |
| +0x5ac | Turning speed factor (load, ailments, ground) |

Player update: `FUN_001befa0`. Movement on the ground `FUN_001c2db0`, looking up and down `FUN_001c27c0`, attacks
`FUN_001c1900` (melee `FUN_001c23e0`, gun `FUN_001c1a00`, bow `FUN_001c2190`).

**The game takes no attack of any kind while the equipped weapon's sounds are not loaded** (`FUN_0012c400(3)` = the
byte at 0x201ed7). If attacks do nothing, look there first; STAR logs when that byte changes.

Things that stop the player's input: 0x201ea0 set (an event), 0x201eb8 set (the game's own menu).

### Items

The game keeps **two** lists, and giving an item to the wrong one makes it invisible:

- What the player **carries** (the Item and Equipment menus): 4 bytes an item at the player's item record
  (`*(0x201e20)`) + 0xa0 + 4 x number; ten equipment slots as half words at +0x80.
- The **store**: 8 bytes an item at save data + 0x5c00. Save data = `*(0x201dc0)`.

Functions: `FUN_001cbed0(item, how many, durability)` gives into the hands; `FUN_001cbcd0(item)` the same for one
armour piece or weapon; `FUN_001cc0c0(item, 1)` = "may the player take one more"; `FUN_001a95a0(item, count)` puts
into the store; `FUN_001a9510(item)` takes out of it.

Definitions: 0x90 bytes each at `*(0x2022b0)` (half word +10: 0 stackable, 1 armour, 2 weapon; +0x20 weapon kind;
+0xc the number of its own magic effect). Numbers: 1-80 usable things, 100- helms, 200- armour, 300- gloves, 400-
boots, 500- bracelets, 600- amulets, 700- rings, 800-1009 weapons. **Enchantments are not rolled:** every enchanted
weapon or armour piece is an item of its own with its own number.

### Maps

The world is maps streamed in as the player walks, two loaded at a time (pointers at 0x201e88 / 0x201e8c; loader
`FUN_001b0020`). The word at 0x2022b8 is the map the player is in. An **area** is a map's number / 10. The game's
own list of its 42 maps is at 0x1f37b0.

**Places are counted from the middle of the map the player is in.** The world is moved when the player crosses into
the next map (`FUN_001b51f0`).

A loaded map's record (0x1e0 bytes): +0 its number, +0x134 pointers to its sections: 0 the map's parts, 1 things
(doors, shrines, chests), 2 creatures (0x54 bytes each), 3 people (format not worked out).

Jumping to a map uses the game's own "start play again from the saved place" (`FUN_0012c0a0`): write the map (save
data +0x2f) and place (+0x30, three 16-bit numbers), then set the words 0x201ea4, 0x201e98 and 0x201e9c to 1. The
restart reloads the player's record from the save data (`FUN_001c9bb0`), so copy the record into the save data first
or anything given since the last real save is lost. A jump into empty space stops the game: use a known place
(`kMapSpots` in `debug_menu.cpp` has one tested place per map).

The line test `FUN_001b3240(map, result, segment, layers, 0)` runs a segment against a map's collision (layer 2 =
floor, 0 and 1 = walls and slopes).

### Creatures

`FUN_001b43a0(map record)` is the keeper, every frame: for each record of the map's creature list, once the player
is near enough and it was not killed too often, it fetches the kind (`FUN_001a7d90`), loads its files and makes it
(`FUN_001a6c90`). Limits: **eleven kinds held at a time** (an unchecked twelfth would overrun), and a fixed number
of creature places.

To put a creature of any kind in front of the player, rewrite a record of the current map and let the keeper do the
rest (`askCreature` in `debug_menu.cpp`). The map's file is read anew when the player leaves and returns.

A creature's "do I notice the player" ends in `FUN_0013e610`: bit 4 of the byte at creature + 0x5b4 makes the answer
no. Twelve kinds kill a standing player within seconds when spawned next to them.

### Sound

One bank of creature sounds is loaded at a time: the **area's** (`se/m%03ds.mse`). `FUN_0012c4d0` swaps it when the
player's area changes. A creature of another area is silent unless its area's bank is loaded in place of the current
one, which silences the current area's own creatures and standing sounds meanwhile. Sounds are numbers below
0x2800; four bytes each at 0x222bc0; `FUN_00131640` starts one.

### Effects

A muzzle flash is not part of the gun: it is an effect. The weapon's animation event calls `FUN_001d1f20`, which
makes an emitter at a bone and never moves it again (the game leaves a shot's effect in the world). Emitters: 0x100
of 0xed0 bytes at 0x3407e0. Particles: 0x120 bytes each at `*(0x201fc0)`; each has a model's record or a sprite's.
Two traps met here: the bits that tell a sprite's record differ from map to map, and in some maps the particle
records are filled after the models are drawn.

### Text

`FUN_001875c0(f12 = x, f13 = y, f14 = size, a0 = text, a1 = 1 centred / 0 from x, a2 = colour)` draws a text.
`FUN_00195b10(index)` fetches one from a table. `FUN_001256e0` is sprintf.

Text tables (always in memory in play) have the shape: "STA ", count, 8 zero bytes, count x {number, offset of the
name, offset of the description}, then the texts. Items at `*(*(0x202160))`, creatures `*(*(0x202164))`, areas
`*(*(0x20215c))`.

The English patch's wording is kept everywhere. Two layout faults of the patch are corrected by setting
`text_fixes`: the Item Repair cost drawn on top of its label (`FUN_0019f050`; fixed in the draw hook), and a damaged
letter in four item texts (items 17, 301, 904, 968).

### Shadows

Only creatures and people cast real-time shadows; the scenery's are in its textures. They are **shadow volumes**
counted in a colour buffer, every frame after the scene:

1. The scene's depth is drawn into a 256x256 buffer (page 0x16e), another 256x256 buffer (page 0x15e) is the count.
2. About 2,000 untextured strips add and subtract in the count, depth-tested, drawn by VU1 program 0x678. Per
   triangle facing the light it draws a closed prism (the triangle, three sides, the far end).
3. Four sprites turn the count into a one-bit mask.
4. One sprite lays the mask over the picture (the picture times 0.375 where set).

Entry points: the shadow loop `FUN_0016c350` calls `FUN_0016ded0` then `FUN_00132a70`, which sends the piece's
shadow mesh and a 34-quad-word header (`FUN_001323c0`) and starts program 0x678.

Why they look the way they do, and what STAR's options do about it:

| What you see | Cause | Setting |
|---|---|---|
| Stepped, blocky edges | The mask is 256 pixels across a 640-pixel picture, and the depth passes through 16 bits | `sharp_shadows`: the two buffers are drawn at the picture's resolution and the depth is taken straight from the scene's depth buffer |
| A creature's shadow vanishes when it is right beside you | The GS cannot clip, and program 0x678 drops a whole prism when one corner is off screen. The game's visibility test `FUN_0016bd00` also sweeps the wrong way along the light | `near_shadows`: the dropped prisms are rebuilt on the PC, clipped (`shadow_volumes.cpp`) |
| Hard patches on a creature's own body that jump as it breathes | Self-shadowing from a single shadow light per room that does not agree with the shading, on low-polygon bodies | `self_shadows = off`: every draw is labelled with the object it belongs to, and a volume is not counted on its own caster |
| The same shadow two or three times (on a pillar's far side, on the ground behind it) | A volume is a long prism that passes through scenery; nothing stops it | `shadow_walls`: not counted on surfaces turned away from the light, nor behind scenery seen on screen |

## 5. STAR's features and how each is done

### Smooth motion (`smooth_60fps`, shown as "Framerate: Unlocked")

The game's logic stays at 30. For each game frame, extra pictures are drawn from the game's own drawing stream with
the camera and every object moved part of the way from the previous frame to this one, one per screen refresh
(`frame_double.cpp`, running on the VIF worker thread; the renderer keeps and presents them at their times).

- Objects are paired between frames by a hash of their vertex data, with rules that tell copies of one model apart.
  A wrong pairing sweeps an object across the screen for one frame: every pairing rule in the file exists because of
  a reported flicker.
- Texture uploads are repeated in every extra pass: the game loads different textures to the same place within one
  frame.
- The rate adapts: when a picture per refresh does not fit, every second or third refresh gets one.
- **The self-test:** `STAR_SMOOTH_SELFTEST=1` places every extra pass at the frame's own state, so all pictures of a
  frame must be identical; `tools/selftest.py` compares them. Run it after any change to the passes.

### Low-delay mouse look (`mouse_live_view`)

The mouse is read about every millisecond, and each picture's camera is turned at the last moment to where the
mouse is. To keep pictures whole, the game's "in view" test is replaced while the mouse turns (`inViewHook`): the
view is widened by margins toward where it is turning, and each picture's camera is kept inside them. A turn too
fast for the margins costs delay, never a broken picture.

The player's own pieces (arms, weapon, muzzle flash) must turn with the camera or they slide. Which pieces are the
player's is **asked of the game** (`ownDrawObject`: the body's draw object, first-person models, equipped weapons),
not guessed from how things move. Guessing took three failed rounds.

### Reduced input lag (`late_input`)

The pad read is moved from the start of the frame to just before the logic, and the pad state is written fresh into
libpad's buffer at that moment. From a key going down to its frame being handed over: 79 ms on average before, 34
after. It falls back to the usual timing when a frame runs long.

### True analog aiming (`analog_aim`)

In the game, turning on the ground starts only past 40% stick and at a fifth of full speed; looking up and down has
one speed. The hooks on `FUN_001c2db0` and `FUN_001c27c0` read the raw stick and move yaw and pitch in proportion,
up to the game's own top speeds times the settings. Needs a control type that looks with the right stick.

### Mouse and keyboard, and rebinding

While the keyboard or mouse was used last, the game is told control type 4; the controller keeps the player's own
type. The mouse's buttons become the game's buttons in `mouseKeysPlay` (`game_fixes.cpp`), with waits so a click is
not lost while the game cannot take it.

Controls are rebindable (`bindings.cpp`, lines `bind_<action>` in the settings file). Actions on one PS2 button
become player 1's key bindings in the runtime, so they work in the game's menus; gun, bow and swing actions are read
by the mouse code. A recorded regression of scripted clicks in all three swing styles guards this code: see
section 8.

### The launcher and the settings table

`options.cpp` holds every setting once: its name in the file, where it shows, wording, choices, default, and what
each preset sets. Add a setting there and both the launcher and the F2 menu can show it. The default of every
setting is the STAR preset's value, so a fresh install starts on that preset. The program applies the table's
default to any setting missing from the file (`loadSettings` in `main.cpp`).

### The Debug page

`debug_menu.cpp`: give items (into the carried list), jump to any map or a saved spot, spawn creatures where the
view meets the ground. It calls the game's own functions from the game thread.

## 6. Known issues

- **Creature self-shadows are wrong in the game itself.** `self_shadows = off` hides them; a true fix was tried
  twice and withdrawn (a depth offset moves with the camera; "lit side only" made little difference).
- **"Shadows stop at walls" only knows scenery that is on screen.** A thin streak can remain.
- **A second monitor with another refresh rate:** the pacing asks Windows for the compositor's (primary) screen. A
  window on a second monitor with a different rate is paced to the wrong grid.
- **The first run stalls briefly** (0.5-0.8 s in the first seconds, and the first time each effect appears) while
  shaders compile. Later runs use the cache.
- **Very fast flicks** (from about 380 degrees a second): the view's speed dips for two or three frames mid-flick.
- **In-between pictures leave out** 2D effects the game projects itself and texture animation; those step at 30.
- **Menu and movie animation stays at the game's own rate.** Only 3D scenes get in-between pictures.
- **Spawned creatures:** killing one and spawning again, saving with one, and carrying one into the next map were
  not tested. A creature of another area is silent unless its sounds are loaded in place of the area's.
- **The F1 recorder** writes about 250 MB per catch (512 uncompressed pictures at 480x360) into
  `logs/pictures_<n>` under the working folder. The setting `record_keep` (5 by default) deletes the oldest.
- **People (NPCs) cannot be spawned:** their record format was not worked out.
- The IPU can hold more unconsumed input than its register can state while a command waits (logged once a run; no
  loss seen in the movies).

## 7. Hard limits

- **Game logic is 30 a second and cannot simply be doubled.** The frame time is an inline constant in dozens of
  places. Smoothness comes from extra pictures, not from faster logic.
- **About one frame of picture delay is built in.** The game sends a frame's drawing one frame after reading its
  input, and an in-between picture lies between the frame before and this one. The mouse view escapes this; walking,
  clicks and weapon switching do not. Handing the frame to the renderer as soon as it is built would need the play
  loop reworked.
- **One extra picture costs about one more run of the whole frame** through the VU1 interpreter (2-6 ms). Heavy
  scenes on fast screens get every second refresh.
- **The game culls for its own camera.** Any picture from a camera turned further than the margins allow would lack
  scenery or wrap triangles. This bounds how far ahead of the game a picture's view can be.
- **Only one version of the game is supported**, by hash. Every hook address would have to be found again for any
  other build.
- **Vulkan only** for the shadow options (the DX11 path has stubs).

## 8. Testing without a screen

Most of STAR was built by an assistant that cannot see the screen or press a key. The method that worked:

1. **Reproduce from a save state** of the player's, with their settings, by scripted input.
2. **Take pictures from inside the renderer**, never from the screen, and compare them with scripts.
3. **Measure before and after.** A fix is not done until a number or a picture says so.
4. **Ask the game.** Where the game knows a thing (which pieces are the player's, which object a draw belongs to),
   read it from the game rather than inferring it.
5. **Test in more than one map,** and where the player actually plays. Two "fixes" worked only in the one scene
   they were tested in.

A run is hidden with `--hidden`, capped with `--seconds <n>` (exit code 124), and takes `--iso <path>`. Hidden runs
do not read the settings file (unless `STAR_TEST_SETTINGS=1`) and play on a copy of the memory cards.
**Test output piles up fast** (uncompressed pictures): delete it when a test is done.

Switches used most (environment variables; hundreds more are named in code comments):

| Switch | Use |
|---|---|
| `PS2X_STATE_LOAD_AT=<field>:<file>`, `PS2X_STATE_SAVE_AT` | Start a run from a save state |
| `PS2X_PAD_SCRIPT="<field>v:<button>:<fields>,..."` | Scripted pad (`up`, `circle`, `lleft`, `rx<n>` ...) |
| `STAR_KBM_TEST=<from>-<to>:<dx>:<dy>:<buttons>:<wheel>,...` | Scripted mouse and (bits 8+) keys, per game frame |
| `STAR_LIVE_TEST=<x>:<y>[:<from>:<to>[:<swing>]]` | A steady or swinging test mouse in real time |
| `STAR_KBM_LOG=1` | What the mouse code added to the pad each frame |
| `PS2X_PCSX2_SNAPSHOT_DIR`, `PS2X_PCSX2_SNAPSHOT_EVERY` | Renderer snapshots (BMP; `tools/bmp2png.py`) |
| `PS2X_PCSX2_PRESENT_DUMP=<folder>:<first>:<last>` | The pictures exactly as presented |
| `PS2X_PCSX2_RING_SAVE_AT=<seconds>:<folder>` | An F1 catch in a hidden run |
| `STAR_REFRESH_HZ=<rate>` | Pretend a screen of that rate |
| `STAR_SMOOTH_SELFTEST=1` | The self-test of the extra passes |
| `PS2X_CALL_WATCH=<hex value>` | Every call whose arguments hold that value: finds the functions that handle a thing |
| `PS2X_CALL_WATCH_FN=<hex function>` | The first calls of a function with their arguments |
| `PS2X_GS_RENDERER=native` + `PS2X_GS_TRACE=<tick>:<count>:<file>` | A list of every draw of a frame |
| `PS2X_GAME_MENU_OPEN_AT=<ticks>`, `STAR_MENU_PAGE_FIRST=<page>` | Open the F2 menu by itself on a page |
| `STAR_SETTINGS=<file>` | Another settings file, so a test never touches the player's |
| `STAR_LAUNCHER_SHOT=<png>` (+ `_PAGE`, `_SCROLL`, `_PRESET`, `_SET`, `_CONTROLS`, `_BIND`) | Pictures and actions in the launcher |
| `STAR_DEBUG_TEST=<frame>:give:<item>:<n>,<frame>:go:<map>:<x>:<y>:<z>,<frame>:creature:<kind>` | The Debug page's actions by script |

The play log `build/bin/logs/play_last.log` is the first thing to read in any report: a `[stutter]` line for every
late vblank says where the time went, and the `controls (...)` line lists the bindings in use.

The F1 recorder (`record_pictures`) keeps the last few seconds of pictures; F1 writes them with `list.txt` and
`frames.txt` (per game frame: the pictures' ids, the camera, every object and how it was paired). For a flicker
seen once, that folder is the evidence: `tools/ringsteps.py` finds the odd pictures.

**The mouse regression.** Before changing `mouseKeysPlay`, record a baseline: scripted clicks from a state with a
gun in hand, in each swing style, with `STAR_KBM_LOG=1`; keep the `[kbm]` lines. After the change the lines must
match. The run repeats exactly, so any difference is yours.

Traps in testing:

- Run timing-sensitive tests one at a time: parallel runs slow each other and the real-time test mouse shows steps
  five times too large.
- Judge the evenness of motion only from runs without picture dumps: the dumps make frames arrive unevenly.
- Sort numbered picture files by number, not as text.
- A snapshot shows the game's buffer; the F2 menu and overlays are drawn after it and cannot be pictured this way.
- An inline shell here-document mangles backslashes and quotes; write scripts to files.

## 9. Settings and defaults

`STAR settings.ini` sits next to the program. Every line is `name = value`; the launcher and the F2 menu rewrite
lines in place and keep the comments. `tools/stage_settings.sh` stages the file for a commit with every value at its
default, so a developer's own values never go into git; keep its list in step with the defaults in
`src/star/options.cpp` and `src/star/bindings.cpp`.

Presets (Original, Remastered, STAR) are defined by the `presets` column of the options table. The launcher keeps
the chosen one in `preset =`; any change of any setting makes it Custom.
