# OptiCraft — webOS Edition

Port of **OptiCraft NetCast Edition** (OptiCraft Heritage 1.7.3) to LG **webOS**
TVs. The webOS platform layer is built on **SDL2** — the same approach as the
[McBetaCpp webOS port](https://github.com/ballslober12/McbetacppWebos) — with an
OpenGL ES 1.x renderer and full **gamepad support** through SDL's
`SDL_GameController` API, alongside the Magic Remote.

Everything game-side comes from OptiCraft Heritage. What is new is `src/webos/`
(the window, GL context, input, audio wiring, packaging) plus a `WEBOS_PLATFORM`
target in the build.

## Status

- Cross-compiles and packages cleanly with the webOS SDK toolchain
  (`arm-webos-linux-gnueabi_sdk-buildroot`, GCC 14.2).
- `dist/com.opticraft.webos_1.7.3_arm.ipk` is produced by `webos-package`.
- The binary was built and inspected (32-bit ARM EABI5, stripped), but **it has
  not been run on a TV here** — no device was available in this environment.
  Expect the usual first-run bring-up: check `webos.log` if anything is off.

## What you need

| | |
|---|---|
| Toolchain | `arm-webos-linux-gnueabi_sdk-buildroot` (buildroot-nc4 / webOS SDK), CMake 3.21+ |
| SDL2 | downloaded at configure time from [webosbrew/SDL-webOS](https://github.com/webosbrew/SDL-webOS/releases) (`release-2.30.12-webos.5`, pinned by hash). It is **bundled** in the IPK because the TV's own libSDL2 may lack the Wayland/xkbcommon backends. |
| GLES1 | `libGLESv1_CM` from the webOS sysroot; it stays on the TV and is not bundled. |
| Packaging | `ares-package` (LG webOS CLI or webosbrew `ares-cli-rs`), optional `webosbrew-gen-manifest` |
| Game data | the Minecraft **assets** folder (or a single `assets.pak`), see below |

### Game data

The game needs the Minecraft asset tree (`assets/minecraft/...`, sounds under
`resources/...`). It is not in this repository. Two ways:

1. Drop an `assets/` folder next to the executable — it is packaged as-is.
2. Pack everything into one `assets.pak` with `scripts/make_pak.py` (the NetCast
   build uses the same pak); put `assets.pak` next to the executable. The
   runtime mounts it automatically (`PlatformResources::resolveExisting`).

If neither is present the game starts but every texture/sound lookup fails.

## Build

The build mirrors the McBetaCpp webOS port:

```sh
cmake -B build/webos -DPLATFORM=WEBOS -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/root/webos/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
cmake --build build/webos --parallel
cmake --build build/webos --target webos-package
```

The IPK is written to `dist/`. On a Windows host the previous IPK can be held
open by Explorer or the search indexer; the build then writes the fresh one as
`dist/com.opticraft.webos_<version>_arm-new.ipk` instead of failing (close the
preview / let the indexer finish and the next run replaces the canonical name).
On a Windows host the whole sequence can be run inside WSL (that is how this
port was built):

```sh
wsl -d Ubuntu-26 -- bash -lc "cd /mnt/d/.../OpticraftWebosEdition && ./build\ webos.sh"
```

`build webos.sh` (in the repo root) wraps the three commands and honours
`WEBOS_SDK` (defaults to `/root/webos/arm-webos-linux-gnueabi_sdk-buildroot`).

CMake options for this target:

| Option | Default | Meaning |
|---|---|---|
| `PLATFORM=WEBOS` | — | selects `cmake/webos.cmake` |
| `WEBOS_LOWEND_PROFILE` | `ON` | same low-end profile the NetCast build uses (`PC_LEGACY_BUILD`): small chunk radius, budgeted chunk/light work, 16-bit terrain vertices, no particles/rain |
| `WEBOS_LTO` | `OFF` | link-time optimisation |
| `MC_LOG_LEVEL` | `0` | 0=off … 3=trace |
| `WEBOS_MANIFEST_ICON`, `WEBOS_MANIFEST_LINK` | empty | when both are set and `webosbrew-gen-manifest` is installed, a Homebrew Channel `manifest.json` is generated next to the IPK |

## Install on the TV

Homebrew Channel, `ares-install`, or any IPK installer:

```sh
ares-install dist/com.opticraft.webos_1.7.3_arm.ipk
ares-launch com.opticraft.webos
```

The app id is `com.opticraft.webos`, the executable is `OptiCraft`, and the
bundle ships `lib/libSDL2-2.0.so.0` next to it (`RPATH=$ORIGIN/lib`).

### Files at runtime

| Path | What |
|---|---|
| `<app dir>/assets/` or `<app dir>/assets.pak` | game data |
| `<app dir>/resources/` | sounds and skins |
| `<app dir>/webos.cfg` | optional settings, see `deploy/webos/webos.cfg` |
| `$HOME/.opticraft/` (or `/tmp/opticraft`) | writable data: options.txt, `saves/`, screenshots. The app bundle is read-only, so this is where worlds live. Override with `OPTICRAFT_DATA_DIR`. |
| `$HOME/webos.log` or `/tmp/webos.log` | log (falls back from the app folder when it is read-only) |

Keep `webos.log` when reporting a problem: it has the SDL/GL strings, the CPU
and memory lines, and a 5-second fps/CPU report.

## Performance

A 1080p TV panel on a tile-based GPU is fill-rate bound, so the frame is drawn
into an offscreen colour texture at a fraction of the panel size and stretched
to the window in one quad (`src/webos/render/WebOSGlesRenderBackend.cpp`, API in
`src/webos/WebOSRender.h`). At 67% scale the GPU shades 55% of the pixels, at
50% it shades 25%.

Defaults are tuned for a weak TV:

| Knob (`webos.cfg`) | Default | Effect |
|---|---|---|
| `render_scale` | `0` (auto) | offscreen fraction: `0` auto, `1` native, `2` 80%, `3` 67%, `4` 50%, `10..100` percent |
| `target_fps` | `30` | what the automatic mode aims for |
| `auto_min` | `50` | lowest scale auto mode may pick |
| `use_vbo` | `1` | display-list geometry kept in ES buffer objects instead of resubmitted client arrays |
| `prof_finish` | `0` | `glFinish()` before the swap is off; it serialises CPU and GPU |
| `gl_errors` | `1` | polls `glGetError()` only every 30th frame for the first seconds |
| `render_distance` / `render_distance_fine` | game's own value | chunk / block view distance override |
| `view_radius` | `3` | renderer grid radius in chunks (`2` = 5x5, `3` = 7x7, `4` = 9x9); each section is at least one draw call |
| `connected_textures` | `0` | OptiFine CTM: each connected group is a separate mesh *and* draw call per section |

Automatic mode starts at 75%, waits out the first seconds of world loading,
then walks the scale between 50% and 100% to hold the target frame rate; every
change is logged as `[SCALE] … (… ms/frame, target N fps)`.

If it is still slow, in order: set `render_scale=4` (50%), lower
`target_fps` to `24`, set `render_distance=1`, and turn texture blur/mipmaps
off in the game's video settings. If geometry is missing or the picture is
garbage, set `use_vbo=0` (driver bug) or `render_scale=1` (no offscreen pass).

Screenshots are taken at the offscreen size while scaling is active.

## Controls

Full tables in [CONTROLS.md](CONTROLS.md). Short version:

- **Gamepad**: left stick walk, right stick look, RT attack/mine, LT use/place,
  A jump, B Q / Escape, X or Y inventory, LB/RB hotbar, Start pause, Back player
  list, L3 sneak, R3 sprint, D-pad cursor + on-screen keyboard navigation.
- **Remote**: arrows move the pointer in menus and walk/turn in the world, OK
  clicks/attacks, Back = Escape, digits = hotbar, red/green/yellow/blue =
  use/jump/inventory/sneak, CH+/CH- = hotbar.
- **On-screen keyboard**: appears for chat, server address, world name/seed,
  signs; D-pad down opens it when a text field is focused.

## How the port is wired

`src/webos/`:

| File | Role |
|---|---|
| `WebOSPlatform.cpp` | SDL2 init, ES 1.x window, the event pump (remote keys, mouse/pointer, `SDL_GameController`), `webos::log`, `webos.cfg`, frame stats, `makeOpaque` |
| `WebOSOsk.cpp` | in-game on-screen keyboard (d-pad + buttons), drawn over the GUI |
| `WebOSRender.h` | render-scale API (auto/fixed scale, viewport mapping) |
| `render/WebOSGlesRenderBackend.cpp` | the `PcOpenGlRenderBackend` namespace implemented on OpenGL ES 1.x (display lists, GL_QUADS expansion, no occlusion queries) plus the offscreen render-scale pass |
| `lwjgl/{Display,GLContext,Keyboard,Mouse}_webos.cpp` | the four platform facades the shared game code uses |
| `JavaNetwork_webos.cpp` | POSIX TCP backend for multiplayer (replaces the SDL_net desktop one) |
| `CrashHandler_webos.cpp` | crash text into the log |
| `main_webos.cpp` | entry point |

Platform backends selected for this target (`cmake/webos.cmake`):
`RenderAPI_PC` + `RenderTerrainAPI_PC` on top of the ES1 backend, desktop
`SoundManager_PC` (SDL audio), `StorageBackend_PC`, and webOS versions of
`Resources`, `InputBackend` and `ClientPlatformPolicy`.

Shared game code gained a `WEBOS_PLATFORM` term next to every `NETCAST_PLATFORM`
hook (TV-class tuning: autosave period, exact block collisions, bounded frame
catch-up, direct analog movement, software cursor, panorama sampling,
`cull_distance`). Those hooks now include `platform/TvPlatform.h`, which aliases
either the NetCast or the webOS namespace as `tvc`.

## About OptiCraft Heritage

OptiCraft Heritage is a heavily modified, clean-room C++ implementation of
classic Minecraft-era gameplay designed around portability, low-end hardware,
and console-specific optimization. It is not a line-for-line source translation:
runtime, platform layers, rendering paths, input backends, storage, UI, asset
loading, memory policies and console support are its own.

- Targets in this tree: PC (SDL2 + OpenGL), PlayStation 2, Nintendo Wii,
  LG NetCast 4.x, and now LG webOS.
- Clean-room implementation; no original proprietary game source is used.
- Not affiliated with, endorsed by, or sponsored by Mojang Studios or Microsoft.

The upstream build instructions for the other targets are unchanged:
`cmake --preset gcc-release`, `cmake --preset ps2-release`,
`cmake --preset wii-release`, and `-DPLATFORM=NETCAST` with
`cmake/netcast.cmake`.

## Third-party software

Third-party libraries stay under `external/` with their own licenses: SDL2
(zlib licence), zlib, stb, miniaudio, glad, gzip-hpp. Review those licenses
before redistributing binaries.
