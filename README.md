# M33 (March33)

[PPSSPP](https://github.com/hrydgard/ppsspp) (PSP emulator) for jailbroken PS4 homebrew, built in WSL
Ubuntu with the Resonance / DolphinPS4 / Crema PS4 toolchain (clang 21 + libc++ 21 on OpenOrbis).

Status: **v00.01 builds and packages; not yet tested on a console.**

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
  touchpad click = SELECT, **L3 + R3 (or L2) = the in-game menu**, R2 = fast-forward. Everything is remappable in
  Settings > Controls.
- Audio: sceAudioOut, 48 kHz (PPSSPP's mixer resamples).
- Video and audio decoding: FFmpeg 7.1 (the rpcs3 port's PS4 build: H.264, ATRAC3/3+, MP3, AAC).
- UI: PPSSPP's own (controller friendly, device type TV), in the console's language.

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

Installing a firmware (Sony's files, not included): open a game's info screen and choose Install
PSP firmware update (most discs carry an updater; Settings also offers to do it from discs by
itself), or put an official update's `EBOOT.PBP` in a folder under `/data/March33/games` and select
it in the game browser. It is installed to `/data/March33/PSP/NAND`.

## Branding

- Home screen tile and background (`sce_sys/icon0.png`, `pic1.png`) and the in-app icon
  (`assets/ui_images/icon.png`): a PSP-1000-style handheld modelled and rendered in Blender 4.5
  (`scripts/blender/psp_scene.py`, no Sony assets or logos), composed by `scripts/make-icon.py`.
  Re-render: `~/tools/blender-4.5.14-linux-x64/blender -b -P scripts/blender/psp_scene.py -- <out> brand/intro-still.png art/Michroma-Regular.ttf 256 1024 macro_cut macro_wide`
- In the app: the opening animation (`ps4/PS4Brand.cpp`), the M33 logo and intro lettering as
  vectors in the UI atlas (`scripts/make-brand.py` -> `brand/m33-ui.svg`, merged by
  `scripts/brand-stage.py` when packaging), the March33 theme (`assets/themes/march33.ini`), the
  menus' background (`assets/m33_background.jpg`) and "M33" in the menu texts (PPSSPP keeps its
  credit in the About screen and the opening). `tools/` previews the atlas images and the opening.

## Not yet

- Testing on a console (first boot, then games). Watch `boot-trace.log` for the milestones.
- Text input: PPSSPP's text fields need `INPUT_TEXT_MODAL` through sceImeDialog (Resonance's keyboard).
- HTTPS (achievements, downloads): naett's curl backend loads libcurl with `dlopen`; the sysroot's
  static curl could be wired in instead.
- A home screen of its own: Resonance's launcher themes (cover flow, XMB) could front PPSSPP's UI.

## License

GPL-2.0-or-later, as PPSSPP ([LICENSE](LICENSE)): `patches/ppsspp-ps4.patch` is a modification of
PPSSPP, and the PS4 frontend it adds (`ps4/`) is part of that work. The PS4 runtime in `port/` and
the toolchain scripts come from the Resonance / DolphinPS4 / Crema ports; `port/mesa/ac_ps4_drm.c`
is the RADV winsys of the PS4 Mesa port (MIT, as Mesa); `port/third_party` keeps its own notices.
`art/Michroma-Regular.ttf` is under the SIL Open Font License. No PSP firmware, Sony system files or
games are included: supply your own.
