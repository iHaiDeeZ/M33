# M33 (March33)

By **ShiroKlein** ([@iHaiDeeZ](https://github.com/iHaiDeeZ)) · [github.com/iHaiDeeZ/M33](https://github.com/iHaiDeeZ/M33)

[PPSSPP](https://github.com/hrydgard/ppsspp) (PSP emulator) for jailbroken PS4 homebrew, built in WSL
Ubuntu with the Resonance / DolphinPS4 / Crema PS4 toolchain (clang 21 + libc++ 21 on OpenOrbis).

Status: **1.00**: runs on the console. PSP games (ISO, CSO, PBP) with the PSP firmware's XMB and
game boot, a controller-friendly quick menu (L3 + R3) with save-state pictures and cheats, rewind
(L2), experimental frame generation (30 to 60 FPS), FSR sharpening, RetroAchievements, and the
PS4's own keyboard. Build it with the steps below; the package bundles nothing of Sony's except
the startup clip you render yourself (kept out of the repository).

- Graphics: PPSSPP's Vulkan renderer on DolphinPS4's RADV/GNM port. RADV is linked in and has no
  window system, so `ps4/PS4VulkanWSI.cpp` (Resonance's `ps4_wsi.cpp`) adds `VK_EXT_headless_surface`
  and `VK_KHR_swapchain` on VideoOut: linear 1920x1080 images registered as VideoOut buffers,
  flipped GPU-side behind each frame (`ac_ps4_submit_flip`). PPSSPP sees a normal surface
  (`WINDOWSYSTEM_HEADLESS`) and loads every entry point through it (`VulkanLoader.cpp`). No OpenGL.
- CPU: PPSSPP's x86-64 JIT. Its code buffer is RWX memory within reach of the eboot's code
  (`Common/MemoryUtil.cpp`), as Resonance's dynarec.
- Memory: fast memory over PS4 direct memory with aliasing (`Common/MemArenaPS4.cpp`): one block of
  direct memory, mapped into a reserved 4 GiB range as often as the PSP mirrors it, in 16 KiB pages.
  The fault handler reads registers through the PS4's signal context layout (`toolchain/m33-overlay.sh`).
- Input: DualShock 4 (ScePad), one per signed-in user, with PPSSPP's default pad mapping:
  Cross/Circle/Square/Triangle, D-pad, left stick, L1/R1 = L/R, OPTIONS = START,
  touchpad click = SELECT, **L3 + R3 = the in-game menu**, **L2 (hold) = rewind**, R2 = fast-forward. Everything is remappable in
  Settings > Controls.
- Audio: sceAudioOut, 48 kHz (PPSSPP's mixer resamples).
- Picture: renders at 4x (1920x1088, the TV's 1080p) with AMD FidelityFX's RCAS sharpening by
  default; for lower resolutions, add FSR-EASU (the upscaler) in Settings > Graphics > Display layout &
  effects > Postprocessing shaders.
  (Stock PPSSPP renders at 1x under an upscaling filter, ignoring the rendering resolution; M33
  keeps the chosen resolution and FSR upscales from it.)
- Quick menu (L3 + R3, `ps4/PS4QuickMenu.cpp`), made for the controller after Crema's in-game menu:
  the paused game behind a dark panel sliding in from the left, rows with icons and a gliding
  highlight, values changed with Left / Right. Resume, Save / Load State (by slot; the slot's picture,
  date and dots for the slots on a card beside the panel, as Resonance's), Rewind (on or off,
  per game: off spends nothing), Cheats
  (a page of their own: each on or off, the game's codes fetched from PPSSPP's CWCheat database),
  Resolution, Game Volume, FPS Counter, Settings (PPSSPP's full settings), Display & Effects,
  Restart Game, Exit. PPSSPP's own pause screen stays for the XMB.
- 60 FPS codes aren't offered as a switch (v00.28-v00.29 had one): GTA ran at ~35-40 fps with
  them (the game in slow motion, its logic tied to frames), and switching them mid-game crashed it.
  They're still in the cheat list for anyone who wants to try.
- Frame generation (experimental, off by default: Settings > Graphics > Frame Rate Control, or the
  quick menu): a 30 fps game shown at 60, a frame made between each two in the refresh that would
  repeat one (GPU/Common/PresentationCommon.cpp). As AMD's frame interpolation without the game's
  motion vectors (the PSP has none): block motion found by comparing the frames at the PSP's own
  size, then the frame between them, a plain cross-fade where they don't match. The real frames
  come one refresh (~16 ms) later; 60 fps games are left alone.
- Rewind (`ps4/PS4Rewind.cpp`), as RetroArch's: hold L2 and the game plays backwards, let go and it
  carries on from the picture shown; hold again to go further back. While playing it records a
  snapshot every 10 frames, in a frame with time to spare (at the latest every 30; only the newest whole, the older ones as zstd-compressed 2 KB-block
  differences) and the screen every 2 frames (480x272 RGB565, zstd; blitted down on the GPU and read back a few
  frames late, so the emulator never waits for the GPU: a blocking readback cost 12-27 ms each and
  made games like GTA fall behind, their sound with them). Going back pauses the
  emulator and plays those pictures backwards (1x, up to 3x held), walking the snapshots back with
  them; letting go loads one snapshot. All of it in its own block of direct memory (up to 640 MiB),
  never the app heap. Off in RetroAchievements hardcore mode.
- Video and audio decoding: FFmpeg 7.1 (the rpcs3 port's PS4 build: H.264, ATRAC3/3+, MP3, AAC).
- UI: PPSSPP's own (controller friendly, device type TV), in the console's language. Text fields
  open the PS4's keyboard (`ps4/PS4Keyboard.cpp`, from Resonance; Cross confirms: param.sfo
  ATTRIBUTE 0x2).
- Network: HTTPS (RetroAchievements) through naett's curl backend on the sysroot's static
  curl + OpenSSL, with Mozilla's CA bundle in the package (`assets/cacert.pem`). Host names go
  through libSceNet's resolver (`port/ps4_net.cpp`).

## On the console

| Path | What |
|---|---|
| `/data/March33/games` | games (ISO, CSO, CHD, PBP, ...): the game browser starts here |
| `/data/March33/PSP` | the memory stick: `SAVEDATA`, `PPSSPP_STATE`, `SYSTEM/ppsspp.ini`, `GAME` (homebrew), `TEXTURES`, `Cheats` |
| `/data/March33` | `ppsspp.log`, `boot-trace.log` (startup milestones), `crash.log`, `hang.log` |
| `/data/March33/profile.txt` | if present, the sampling profiler runs (`samples.log`) |
| `/app0/assets` | PPSSPP's assets, in the package |

Title ID `MRCH00033` (content ID `IV0000-MRCH00033_00-MARCH33PSPEMU000`). Install the pkg with
GoldHEN's Package Installer. Games can be copied over FTP to `/data/March33/games`.

## Building

```
scripts/seed-build-tree.sh        # ~/m33-build: Resonance's sysroot and RADV Mesa build, rpcs3's FFmpeg, the ucontext overlay
scripts/clone-ppsspp.sh           # upstream PPSSPP at patches/BASE.txt into ~/m33-build/src/ppsspp (with submodules)
git -C ~/m33-build/src/ppsspp checkout -b ps4 && git -C ~/m33-build/src/ppsspp apply ~/m33/patches/ppsspp-ps4.patch
scripts/rebuild-ppsspp.sh         # configure + build (logs in ~/m33-build/logs)
M33_VERSION=00.01 scripts/package-m33.sh
scripts/save-patch.sh "message"   # commit ~/m33-build/src/ppsspp and refresh patches/ppsspp-ps4.patch
py scripts/make-icon.py brand/concepts3d/macro_cut.png brand/concepts3d/macro_wide.png   # sce_sys art, in-app icon
```

Run the scripts from WSL through `~/m33` (the space in "PS4 Projects" breaks linker flags).
Upload from Windows (Git Bash): `scripts/upload-to-console.sh out/M33-vXX.YY.pkg`. After a crash,
`scripts/crashlog.sh` fetches the logs and symbolizes them against the packaged build's ELF.

## How the port is laid out

The PPSSPP side is `patches/ppsspp-ps4.patch` (26 files):

| File | What |
|---|---|
| `ps4/PS4Main.cpp` | `main()`: system modules, data folders, `NativeInit`, the Vulkan context on the headless surface, the emulator thread, the pad loop; PPSSPP's `System_*` functions |
| `ps4/PS4VulkanWSI.cpp` | the surface and swapchain on VideoOut (from Resonance) |
| `ps4/PS4Pad.cpp`, `ps4/PS4Audio.cpp` | ScePad and sceAudioOut |
| `ps4/ps4.cmake` | the app's sources, RADV, the PS4 libraries, `eboot.bin` |
| `Common/MemArenaPS4.cpp` | the PSP memory arena on direct memory |
| `Common/MemoryUtil.cpp` | RWX JIT memory near the code, 16 KiB pages |
| `Common/GPU/Vulkan/*` | linked-in driver loading, the headless surface |
| `ppsspp_config.h`, `CMakeLists.txt`, `ext/CMakeLists.txt` | `PPSSPP_PLATFORM(PS4)`, no GL / SDL, glslang's OS layer, the adhoc sockets |
| `Core/HW/*`, `Core/HLE/sceMpeg.cpp`, `Core/AVIDump.cpp` | `const AVCodec *` for FFmpeg 5+ |
| `UI/NativeApp.cpp` | assets from `/app0/assets`, the memory stick in `/data/March33` |

The PS4 runtime in `port/` (heap, crash and hang logs, profiler, DNS, network shares, the RADV
winsys) and `toolchain/` come from Resonance; `port/ps4_net.cpp` defines `gethostbyname` itself
(PPSSPP's adhoc server uses it, and the C library's version imports a function the console lacks).

## The PSP's XMB

PPSSPP (1.21 on) can boot the real XMB from an installed PSP firmware (any of 1.50 to 6.61):
Settings > System > PSP Firmware > Launch XMB. M33 adds **Start in the XMB** there: the app goes
straight into it after its opening, and leaving it (or L3 + R3 > Exit to menu) comes back to M33's
menus. Hold **L1** while M33 starts to skip it once.

What it shows: the XMB's Game column lists the memory stick's games (`PSP/GAME`) and, as CFW ISO
drivers did, **every disc game in `/data/March33/games`** (ISO, CSO, CHD) with its own icon and
background: before the XMB boots, M33 writes a stand-in for each (`PSP/GAME/M33ISO_<disc id>/`:
an EBOOT.PBP made of the disc's PARAM.SFO, ICON0 and PIC1, and M33ISO.TXT naming the disc;
`ps4/PS4XmbGames.cpp`). Starting one from the XMB boots the disc (`sceKernelLoadExec`). Saved
Data lists `PSP/SAVEDATA` (= `/data/March33/PSP/SAVEDATA`). The game browser hides the stand-ins,
and they go away with their disc.

Game boot: started from the XMB, a game gets the firmware's own game boot (the emulated PSP plays
it, as on a PSP) and then boots as the disc. M33 adds none of its own: its imitation (v00.16-v00.23)
played on top of the real one and is gone. A disc started from M33's own list goes through the
firmware too, as a PSP powered on with it in the drive: the firmware boots with the disc in and
`umd_autoboot` on (for that boot only), plays its startup and game boot, and launches the disc
(`sceKernelLoadExecVSHDisc`), which M33 then boots as itself, straight in. Without a firmware
installed, games boot straight in.

Known gap: the XMB's Saved Data column doesn't open yet. The firmware reads the memory stick
through its raw partition device (`msstor0p1:`) and a `fatms0:` devctl (0x02425856), neither
emulated by PPSSPP.

Installing a firmware (Sony's files, not included): **Download PSP firmware 6.61 from Sony** on
the same screen fetches Sony's official updater from its update server (32.6 MB, kept in
`/data/March33/firmware`) and installs it. Or open a game's info screen and choose Install
PSP firmware update (most discs carry an updater; Settings also offers to do it from discs by
itself), or put an official update's `EBOOT.PBP` in a folder under `/data/March33/games` and select
it in the game browser. It is installed to `/data/March33/PSP/NAND`.

## Branding

- Home screen tile and background (`sce_sys/icon0.png`, `pic1.png`) and the in-app icon
  (`assets/ui_images/icon.png`): a PSP-1000-style handheld modelled and rendered in Blender 4.5
  (`scripts/blender/psp_scene.py`, no Sony assets or logos), composed by `scripts/make-icon.py`.
  Re-render: `~/tools/blender-4.5.14-linux-x64/blender -b -P scripts/blender/psp_scene.py -- <out> brand/intro-still.png art/Michroma-Regular.ttf 256 1024 macro_cut macro_wide`
- The startup clip: the handheld spins in, powers on and plays a startup video on its screen
  (`scripts/blender/startup_anim.py`, then `encode.py` adds the video's sound; 8 s, 60 fps).
  `package-m33.sh` bundles `brand/startup/m33-startup-preview.mp4` as `assets/startup.mp4`; the
  app plays it (`ps4/PS4StartupClip.cpp`, FFmpeg) instead of the drawn opening, any button skips
  it, and a `/data/March33/startup.mp4` on the console takes its place. It contains the PSP's
  startup video, so it stays out of git (`brand/startup/` is ignored).
- In the app: the opening animation (`ps4/PS4Brand.cpp`), the M33 logo and intro lettering as
  vectors in the UI atlas (`scripts/make-brand.py` -> `brand/m33-ui.svg`, merged by
  `scripts/brand-stage.py` when packaging), the March33 theme (`assets/themes/march33.ini`), the
  menus' background (`assets/m33_background.jpg`) and "M33" in the menu texts (PPSSPP keeps its
  credit in the About screen and the opening). `tools/` previews the atlas images and the opening.

## Not yet

- Testing on a console (first boot, then games). Watch `boot-trace.log` for the milestones.
- A home screen of its own: Resonance's launcher themes (cover flow, XMB) could front PPSSPP's UI.

## License

GPL-2.0-or-later, as PPSSPP ([LICENSE](LICENSE)): `patches/ppsspp-ps4.patch` is a modification of
PPSSPP, and the PS4 frontend it adds (`ps4/`) is part of that work. The PS4 runtime in `port/` and
the toolchain scripts come from the Resonance / DolphinPS4 / Crema ports; `port/mesa/ac_ps4_drm.c`
is the RADV winsys of the PS4 Mesa port (MIT, as Mesa); `port/third_party` keeps its own notices.
`art/Michroma-Regular.ttf` is under the SIL Open Font License. No PSP firmware, Sony system files or
games are included: supply your own.
