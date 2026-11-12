# MHGU Recomp

A native PC port of **Monster Hunter Generations Ultimate** (Nintendo Switch, version 1.4.0), made by statically
recompiling the game's 32-bit ARM code to C and running it on a host runtime: Switch system services and the NVN
graphics API are reimplemented on Windows, with Vulkan for rendering.

**No game files are included.** You need your own copy of the game, dumped from your own Switch. This project does
not explain how to dump it and does not provide game files, keys or links to them.

Status: fully playable, but there may be some bugs. I've played a dozen hunts with no remaining issues. Multiplayer also works.

## What it adds over the Switch version

- Graphics settings (resolution, MSAA, FXAA, Supersampling, VSync)
- Any screen shape from 16:10 to ultrawide (21:9, 32:9 and wider), with the HUD unstretched: centered or spread to
  the screen's sides
- Frame rate cap anywhere between 30-240 fps
- Supports almost every controller under the sun
- Experimental KB+M controls
- Optional: small monsters are properly synced between players in multiplayer
- Optional: monster HP scaling by playercount
- Fast-forward unskippable cutscenes by pressing A
- Optional: village quests are playable in multiplayer (press L/R at the quest counter), with appropriately re-scaled monster HP
- Mod support. The vast majority of mods work by dropping them into the `mods` folder, including Forge mods. (see [Mods](#mods))
- Cheat codes in the same format as Ryujinx and Atmosphère, in the `cheats` folder (see [Cheats](#cheats))

## Requirements

- OS: Windows 10/11 64-bit, or Linux
- CPU: Any CPU from 2014 or newer (needs AVX2 support)
- GPU: Any GPU from 2016 or newer (needs Vulkan 1.2 support), and at least 1GB of VRAM
- The game files for Monster Hunter Generations Ultimate, version 1.4.0. You can extract these from your Switch or Ryujinx with patches pre-applied.
    - (Title ID `0100770008DD8000`, the `main` executable's build ID starts with `FB08F1D20FD1204F`)

## Installation

[Download the latest prebuilt release from the Releases page. Make sure you download the right ZIP file for your desired platform.](https://github.com/morerokk/MHGURecomp/releases)

You need the `exefs` and `romfs` folders from Monster Hunter Generations Ultimate 1.4.0. If you are coming from Ryujinx, you can right-click the game (make sure the 1.4.0 title update is applied), and Extract the game files with the patches pre-applied.

1. Extract the release zip anywhere, for example `C:\Games\MHGURecomp\`.
2. Put the game's `exefs` and `romfs` folders in the `data` subfolder. It should look like this:

   ```
   MHGURecomp\
     mhgurecomp.exe
     mhgugame.dll
     (other .dll files)
     data\
       exefs\
         main
         main.npdm
         rtld
         sdk
         subsdk0
       romfs\
         nativeNX\
         (the rest of the game's romfs)
   ```

   `exefs` must come from the 1.4.0 update, and `romfs` must be the base game's romfs with the update's files
   merged over it.
3. Start `mhgurecomp.exe`.

If the files are missing or the wrong version, a message box says so.

### Where things are

| What | Where |
| --- | --- |
| Settings | `mhgurecomp.ini` next to the exe (written with the defaults on first start; delete it to reset) |
| Save data | `%APPDATA%\MHGURecomp\save\` (kept apart from the game folder so updating the port can't touch it) |
| Log | `log.txt` next to the exe (look here first when something goes wrong) |
| Screenshots | `shots\` next to the exe (F12) |
| Mods | `mods\` next to the exe: one folder per mod (see below) |
| Cheats | `cheats\` next to the exe: `.txt` files with cheat codes (see below) |
| Shader cache | `cache\` next to the exe (safe to delete; the first minutes after that may stutter) |

To move an existing Switch save over, put its `system` and `system_backup` files in
`%APPDATA%\MHGURecomp\save\save_0\`.

## Controls

Almost every controller under the sun should work, as long as it has SDL support. If you really want to swap the face buttons on non-Nintendo controllers, open the .ini file and set `FaceButtons = Label`.

Keyboard and mouse support (not recommended):

| Key | Switch button |
| --- | --- |
| W A S D | left stick (hold Left Shift to walk) |
| Mouse | right stick (camera) |
| Left click / Right click | X / A |
| Space / R | B / Y |
| Q / E | L / R |
| Tab / T | ZL / ZR |
| Enter / Backspace | Plus / Minus |
| Arrow keys | d-pad |
| Ctrl / Alt | left / right stick click |

Click in the game window to capture the mouse for the camera; Escape lets it go (so does switching to another
window). `MouseSensitivity` in the ini's `[Mouse]` section sets its speed; the game's own camera options (speed,
inverted axes) apply to it too.

When the game asks for text (names, chat), the keyboard types: Enter confirms, Escape cancels, Ctrl+V pastes.

Other keys: Alt+Enter or F11 switches between windowed and full screen, F12 takes a screenshot, F7 cycles MSAA.

## Settings

Everything is in `mhgurecomp.ini`; each setting is explained in the file itself. In short:

- `[Display]`: `Mode` (Borderless / Fullscreen / Windowed), `Resolution`, `RenderResolution` (the 3D resolution),
  `AspectRatio` (Auto follows the window: 16:10 and anything wider fill it, narrower gets 16:10 with bars), `FOV`
  (1.0 = the game's), `UIAnchor` (Center keeps the HUD in the middle 16:9 on an ultrawide screen, Edges spreads it
  to the sides)
- `[Anti-aliasing]`: `MSAA` (Off / 2x / 4x / 8x), `FXAA` (On / Off)
- `[Performance]`: `FPS` (30 to 240), `VSync` (On / Off; above your monitor's refresh rate, turn VSync off)
- `[Controls]`: `FaceButtons` (Position / Label), `BackgroundInput` (On / Off: the controller keeps working while
  the game's window is in the background, such as with a wiki in front of it; default On)
- `[Mouse]`: `MouseSensitivity` (1 = default, 0.1 to 10)
- `[Multiplayer]`: `Port`, `Join` (addresses of hosts outside your LAN), `SyncSmallMonsters`
- `[Monster HP]`: HP factors for 1 to 4 hunters in Hub quests, and per-quest exceptions (default: all 1, the
  original game)
- `[Village Quests]`: `VillageQuestsInHub` (default On), `HubHP` (default 2.0: Village quests posted in the Hub
  multiply their monsters' HP by this first, then by the `[Monster HP]` factor)
- `[Bonus Content]`: `BonusContent` (default On), see below

### Multiplayer

Over LAN or a VLAN, this should work out of the box.

Over the internet, the host needs to forward the right port (39500 TCP/UDP by default), the client needs to configure the host's IP address in their .ini file. (`Join = xxx.xxx.xxx.xxx`)

Choose the "Local Wireless" multiplayer options, as this port pretends to be a local nearby Switch with a wireless connection. The online options are unavailable and are unnecessary.

### Bonus content

By default, the `bonus` subfolder in releases contains a savegame with all the free online content (event quests, item packs, palicoes), which will be copied into your savegame when the game starts the 2nd time.

If you don't want this, set `BonusContent = Off` or don't provide a savegame in the `bonus` subfolder. You can also provide your own savegame instead if you want. Note that this port will not attempt to copy bonus content if such bonus content already exists.

### Mods

To install a mod, put the mod folder in `mods\`. Remove the folder to uninstall said mod. Mods are layered over the game at startup.
- romfs mods (files that replace or add game data) are found inside the mod's folder, including the
  `atmosphere\contents\0100770008DD8000\romfs` layout.
- exefs patches (`.pchtxt`, `.ips`) are applied when they change data. Patches to game code can't be applied, because
  the code is precompiled; the log lists them.
- Forge plugins (`.nro` in `romfs\nativeNX\plugins\`) are loaded. Forge's menu with the plugins' settings opens with
  Insert (or the key in Forge's own `config.ini`, if a mod includes Forge's files); use the mouse, keyboard or
  controller in it. While it is open the game gets no controller input. Window positions are kept in
  `forge_imgui.ini`.
- You do not need any resolution, FPS cap or connection fix mods. These features are included with this port.

If you are newer to the game, I very strongly recommend the [Key Quest Markers](https://gamebanana.com/mods/727960) mod by PurplefinNeptuna. This marks out the Village and Hub quests you need to complete for the next tier directly in the quest menu, which means you no longer need a wiki to play the damn game.

### Cheats

Put `.txt` files with cheat codes in `cheats\`. Every cheat in them is on from the start. Codes made for Ryujinx or a
modded Switch work as they are, as long as they are for version 1.4.0. A file can hold any number of cheats, each
starting with its name in square brackets:

```
[Skill points x2]
04000000 000DFAB4 E1A00080
```

- To turn off a file without deleting it, start its name with `_`.
- Cheats that change the game's code also work: the port runs each changed function in an interpreter. That
  function runs slower, which only matters for functions the game calls very often.
- `log.txt` lists the cheats it loaded, and any it couldn't read.
- `extras\cheats\` in the source has two cheats that double the skill points from armor, weapon and talisman
  decorations, and talismans (with Double Talisman, talisman points go x4). `Skill points x2` doubles negative
  points too; `Skill points x2 (positive only)` leaves them as they are. Use only one of the two.
- `Valor Sheathe original damage reduction` sets the damage you take in a Valor Sheathe evade back to the game's
  own values (70% less for Blademasters, 85% for Gunners), for mods that change it, such as GU Rebalanced (50% / 65%).
  The rest of the mod stays as it is. To pick other values, see `docs/research/valor-sheathe.md`.

## Building from source

The build recompiles the game code from your own copy of the game. Nothing in this repository contains game code or
data, and the build never uploads anything.

### 1. Tools (Windows)

1. Install [MSYS2](https://www.msys2.org/) to `C:\msys64`.
2. In the **MSYS2 UCRT64** shell, install the compiler and libraries:

   ```bash
   pacman -S --needed make git diffutils \
     mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-pkgconf \
     mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-nasm \
     mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
     mingw-w64-ucrt-x86_64-vulkan-memory-allocator
   ```

   FFmpeg (for the game's movies) isn't installed: the first build downloads its source and builds the small part the
   movies need into the executable (see "FFmpeg" below).

3. Install [Python](https://www.python.org/) 3.10 or newer for Windows. Then, in a Command Prompt in the repository
   folder, make the tools' virtual environment (the Makefile uses `tools\venv` when it exists):

   ```bat
   py -3 -m venv tools\venv
   tools\venv\Scripts\pip install -r tools\requirements.txt
   ```

### 2. Recompile and build

From the UCRT64 shell, in the repository folder, with your game files in a folder holding `exefs\` and `romfs\` (as
under "Installing a release"):

```bash
make gen GAME=/c/path/to/game      # translate the game code to C in generated/ (~300 MB)
make -j4 GAME=/c/path/to/game      # compile everything into build/
```

The first full build takes a while (about 8 minutes on a 4-core Haswell with `-j4`; more cores: higher `-j`). Later
builds only recompile what changed.

`make` turns `build\` into a ready-to-run game folder, and puts nothing else there (the compiler's own files go
to `obj\`):
- `mhgurecomp.exe`, the runtime;
- `mhgugame.dll`, the recompiled game code;
- the MSYS2 DLLs they need;
- `data\exefs` and `data\romfs`, hard links to your game files, so they take no extra disk space.

To play from another folder, copy the contents of `build\` there; to update that folder, copy them over it (`data\`
can be skipped when it is already there). The build never contains settings, caches, mods or logs, so your
`mhgurecomp.ini` and the rest stay as they are. Or start `run-game.bat`: it runs `build\mhgurecomp.exe` with the
settings, caches, `mods\` and `log.txt` in `run\` (`MHGU_HOME`), keeping `build\` clean. Starting
`build\mhgurecomp.exe` directly also works, but then it writes those into `build\`.

Useful targets and options:

- `make test-lift`: checks the recompiler against the Unicorn CPU emulator on thousands of the game's instructions.
- `make clean`: removes the compiled files (`obj\` and the program files in `build\`). It keeps `build\data`.
- `GENARCH=-march=x86-64-v2`: compiles the game code without AVX2, for older CPUs. Untested.
- `FFMPEG=system`: links the system's FFmpeg libraries instead of building them (see below).

### FFmpeg

The game's movies are MP4 files with H.264 video and AAC audio. The first build downloads FFmpeg's source
(`third_party/ffmpeg/`, a pinned release checked against its SHA-256), builds only the MP4 reader and those two
decoders into `obj/`, and links them into the executable. Nothing of FFmpeg needs to be installed or shipped, and the
Linux build runs on any distribution whatever FFmpeg version it has (or none). This needs `curl` and, for FFmpeg's
fast x86 code, `nasm` (without it FFmpeg uses plain C, which is fast enough for these movies). It takes a few minutes,
once. FFmpeg is LGPL 2.1; its license goes into `build/licenses/`.

`make FFMPEG=system` links the system's avformat / avcodec / avutil instead (FFmpeg 5.1 or newer), as a
distribution package would; that build then only runs with that FFmpeg version.

### Making a release

A release is the contents of `build\` without the game files:
- Include `mhgurecomp.exe`, `mhgugame.dll`, all other `.dll` files and `licenses\`.
- Leave out `data\`.

`make` fills `licenses\` with everything a copy has to carry: this project's `LICENSE.txt`, `THIRD_PARTY_NOTICES.txt`
(code compiled in), FFmpeg's license, and on Windows the licenses of the MSYS2 packages behind the bundled DLLs and
the libraries linked in. `licenses\README.txt` lists those with their versions and where their exact source is, and
holds a written offer for the source of the LGPL DLL among them (libiconv, which SDL3 uses): whoever publishes the
release makes that offer, so keep those sources at hand for three years (the MSYS2 address it gives is one copy).

The exe creates the `mods` and `cheats` folders and the ini on first start. Players then add their own `data\` as
described above.

### Linux

The Windows build runs under Proton / Wine. A native Linux build is supported as well: the Makefile picks
`runtime/platform/linux.c` when `uname -s` isn't Windows and everything else is shared. Tested on Gentoo
x86_64 with clang under Wayland (NVIDIA). Besides `make`, `git`, `clang`, `lld` and Python 3.10 or newer it
needs:

- SDL3, the Vulkan loader and headers and shaderc — the same libraries the MSYS2 list above installs, under your
  distro's names — plus `curl` and optionally `nasm` for building FFmpeg (see "FFmpeg" above; no FFmpeg package
  needed);
- `vk_mem_alloc.h` (3.4.0) in `third_party/vk_mem_alloc/`, which the Makefile has on the include path (a
  distro package that installs the header where the compiler looks by default works just as well);
- `zenity` or `kdialog` for the dialog on a fatal error. Without either, the message goes to the terminal and
  to `log.txt` as usual.

```bash
python3 -m venv tools/venv && tools/venv/bin/pip install -r tools/requirements.txt
make gen GAME=../mhgu/game       # as above: the folder holding exefs/ and romfs/
make -j8 GAME=../mhgu/game
cd build && ./mhgurecomp
```

`build/` becomes the game folder exactly as on Windows: `mhgurecomp`, `mhgugame.so` with the recompiled game
code and `data/` as hard links to your game files, nothing else; the ini, caches and `mods/` are written beside
them when it runs (`MHGU_HOME=<folder>` puts those there instead). Saves go to
`$XDG_DATA_HOME/MHGURecomp/save`, default `~/.local/share/MHGURecomp/save` — the folder the Windows build
keeps them in under `%APPDATA%`, same layout, so moving a save across is copying `save_0`.

From Windows, the Linux build can be made in WSL with Arch Linux (`wsl --install -d archlinux`), straight from the
Windows checkout, into its own folders so the Windows build is left alone:

```bash
pacman -S --needed make git clang lld python sdl3 vulkan-headers vulkan-icd-loader shaderc nasm vulkan-swrast
mkdir -p /usr/local/include/vma && cp /mnt/c/msys64/ucrt64/include/vma/vk_mem_alloc.h /usr/local/include/vma/
make -j16 B=build-linux O=obj-linux     # in the repository, under /mnt/c/...
```

Arch has no package for `vk_mem_alloc.h`; MSYS2's (3.4.0) is the one the Windows build uses. `build-linux/data` can
hold links to the game files (`ln -s /mnt/c/.../exefs build-linux/data/exefs`, same for `romfs`, then
`touch build-linux/data/.complete`) instead of a second copy. It also runs there, in a WSLg window, slowly: WSL's
own Vulkan driver (dzn, Vulkan on Direct3D 12) can't create the renderer's pipeline layouts, so use Mesa's software
one with `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json`. WSLg can't capture the mouse (clicks still work as
buttons), and screenshots come out black there.

## Repository layout

| Folder | What's in it |
| --- | --- |
| `tools/` | the recompiler: `lifter.py` (ARM to C), `neon.py` (NEON / VFP), `gen.py` (whole modules), `nso.py`, test and research tools |
| `runtime/` | the host side: CPU state, kernel and threads, Switch services (`services/`, `hle/`), networking (`net/`), the NVN graphics API on Vulkan (`nvn/`), settings, input, mods |
| `docs/` | `STATUS.md`: progress, design notes, reverse-engineering findings, debug options |
| `extras/` | `cheats/`: cheat files players can copy into their `cheats` folder |

## License

MHGU Recomp's own code is under the MIT License (`LICENSE`). It builds on code from LAS Recomp and Forge, and
bundles cimgui and Dear ImGui, all under the MIT License too: see `THIRD_PARTY_NOTICES`. The license does not cover
the game or its code, including the recompiled game code a build produces (`mhgugame.dll` / `mhgugame.so`).

## Legal

This project is not affiliated with or endorsed by Capcom or Nintendo. Monster Hunter is a trademark of Capcom. The
repository contains no game code, data or keys; the build reads them from a copy you provide. Please support the
developers by owning the game.

## Logging, Crash Diagnostics & Minidumps

`MHGURecomp` includes a crash-safe, write-through diagnostic logger (`runtime/diag.c`, `runtime/diag.h`) that initializes before any other subsystem, rotates previous log files, records CPUID/XGETBV/Vulkan/SHA-256 system metadata, tracks startup phases and thread lifecycles, monitors for main-loop hangs via a watchdog thread, and generates symbolicated stack traces plus Windows `.dmp` minidumps on any unhandled exception.

### Log & Minidump Locations
- **Primary Log**: `log.txt` next to `mhgurecomp.exe` (or inside `MHGU_HOME` / `MHGU_LOG_DIR`).
- **Rotated Logs**: Previous runs are preserved as `log.1.txt`, `log.2.txt`, ..., `log.4.txt` (default 5 files kept).
- **Read-Only Fallback**: If the game folder cannot be written to (e.g. `C:\Program Files\...`), logs and crash dumps automatically fall back to `%LOCALAPPDATA%\MHGURecomp\` and record the final path on the first line of `log.txt`.
- **Crash Minidumps**: `crash/crash_<YYYYMMDD_HHMMSS>_pid<PID>_tid<TID>.dmp` (written via `MiniDumpWriteDump` with thread info, data segments, and indirectly referenced memory).

### `mhgurecomp.ini` `[Diagnostics]` Settings & Environment Variables
| `.ini` Key (`[Diagnostics]`) | Environment Variable | Default | Description |
| --- | --- | --- | --- |
| `LogLevel` | `MHGU_LOG_LEVEL` | `INFO` | Verbosity level: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, or `FATAL` |
| `LogConsole` | `MHGU_LOG_CONSOLE` | `Auto` | Mirror log lines to console/`stderr`: `Auto`, `On` (`1`), or `Off` (`0`) |
| `LogKeepFiles` | `MHGU_LOG_KEEP` | `5` | Number of rotated `log.*.txt` files to keep (`1..50`) |
| `WatchdogTimeout` | `MHGU_WATCHDOG_SEC` | `15` | Seconds without frame/loop progress before dumping all thread stacks (`0` = disabled) |
| `VulkanValidation` | `MHGU_VK_VALIDATION` | `Off` | Enable `VK_LAYER_KHRONOS_validation`: `On` (`1`) or `Off` (`0`) |
| `CrashMinidump` | — | `On` | Write `.dmp` minidumps to `crash/` on unhandled exceptions: `On` or `Off` |
| — | `MHGU_LOG_DIR` | — | Override directory for `log.txt` and `crash/` |

### Collecting a Diagnostic Bundle (`collect-diag.bat`)
Run `collect-diag.bat` (or `powershell -ExecutionPolicy Bypass -File tools/collect_diag.ps1`) to package `log.txt`, `log.*.txt`, recent `crash/*.dmp` minidumps, `mhgurecomp.ini`, and hardware/driver/SHA-256 summary into `diag/MHGURecomp_Diag_<timestamp>.zip`. Game ROM files (`data/exefs`, `data/romfs`) and player saves (`save/`) are never included.
