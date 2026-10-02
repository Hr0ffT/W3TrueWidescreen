# W3TrueWidescreen: unstretched widescreen for Warcraft III 1.26a

*[Русская версия](README.ru.md)*

W3TrueWidescreen makes the classic Warcraft III (Reign of Chaos / The Frozen Throne, patch **1.26a**) look right on 16:9, 16:10 and ultrawide screens. It keeps the game's original look: nothing is stretched, redrawn or replaced.

## What it does

- **Unstretched interface.** The in-game console keeps its original proportions and sits in the centre of the screen. The game world fills the full width.
- **The world fills the whole screen**, including behind the console, the way patch 1.29+ does it. The see-through gaps of the console stay black, as in the original.
- **Original field of view.** On a wide screen the engine zooms in (it derives the field of view from the screen diagonal). The mod restores the 4:3 vertical view, so the extra width just shows more.
- **Menus.** The 3D backgrounds fill the screen at the original distance. Buttons and the decorative frames, chains and panels go to the screen edges, as they sit on a 4:3 screen. The loading and score screens stay centred.
- **Hero portraits** go to the left screen edge, and the letterbox of in-game cutscenes spans the full width (both optional).
- **Campaign menu fade** covers the whole screen instead of only its left part.
- **Movies at native resolution.** For every movie the game switches the screen to 800×600 and brightens it with a strong gamma ramp, so on a modern display the movies look blurry and washed out. The mod keeps the desktop resolution and colours.
- **Optional movie renderer** (the `W3TrueWidescreen` folder): the movies are decoded by LAV Filters and drawn by MPC Video Renderer, full screen with clean blacks and, on NVIDIA RTX cards, RTX Video super resolution. The original Blizzard movie files play without installing codecs.
- **NVIDIA App keeps its settings for the game.** The game locks its process against other programs (an old anti-hack measure), so NVIDIA App cannot tell which game is running and forgets RTX HDR and the game filters. The mod lets other programs read only the path to the game's exe; memory access stays locked. The mod is loaded after NVIDIA App's check at game start, so your saved values no longer reset, but the profile is not applied automatically: select it once in the overlay (Alt+F3) after starting the game. For a complete fix, there is a standalone proxy DLL, [W3_RTX_HDR_Fix](https://github.com/Hr0ffT/W3_RTX_HDR_Fix), which hands NVIDIA App the exe path at an early boot stage, so the correct profile is applied immediately on launch with nothing to click.
- Everything happens in memory while the game runs. **No game files are modified.**

## Requirements

- Warcraft III **1.26a** (Game.dll version 1.26.0.6401). The mod checks the version and does nothing on any other patch.
- Direct3D renderer (the default). The mod also works with dgVoodoo2. OpenGL mode (`-opengl`) is not supported: the console backing is skipped there.
- The optional movie renderer was tested on Windows 11 with an NVIDIA RTX card; RTX super resolution needs an NVIDIA RTX card (the rest works on any modern GPU).

## Installation

1. Copy `W3TrueWidescreen.mix` and `W3TrueWidescreen.ini` into the Warcraft III folder, next to `war3.exe`.
2. Optional: copy the `W3TrueWidescreen` folder there as well, for the movie renderer.
3. Set your native resolution in the game's video options (or in the registry: `HKCU\Software\Blizzard Entertainment\Warcraft III\Video`, `reswidth`/`resheight`).
4. Start the game (`war3.exe`, `Warcraft III.exe` or `Frozen Throne.exe`).

The game loads `.mix` files from its folder on its own; no launcher is needed.

**Uninstall:** delete `W3TrueWidescreen.mix`, `W3TrueWidescreen.ini`, `W3TrueWidescreen.log`, and the `W3TrueWidescreen` and `W3TrueWidescreen_cache` folders.

## Settings

All settings live in `W3TrueWidescreen.ini` and are described there. The main ones:

| Option | Default | Meaning |
|---|---|---|
| `WorldFullHeight` | 1 | World behind the console (0 = original black strips) |
| `FovFix` | 1 | Original 4:3 vertical field of view (0 = engine default, zoomed in) |
| `CameraZoomOut` | 1.0 | Extra zoom-out for the game camera |
| `HeroBarEdge` | 1 | Hero portraits at the left screen edge |
| `CinematicFullWidth` | 1 | Cutscene letterbox across the full width |
| `MenuLayout` | 1 | Menu panels at the screen edges (0 = centred 4:3) |
| `MovieNativeMode` | 1 | Movies at desktop resolution without the gamma change (0 = original 800×600) |
| `MovieRenderer` | 1 | Use the movie renderer from the `W3TrueWidescreen` folder if it is there |
| `MovieSuperRes` | 1 | NVIDIA RTX Video super resolution for the movies |
| `MoviePlayer` | 0 | Play movies in an external player: 1 = MPC-HC, or the path to the player's exe |
| `AllowPathQuery` | 1 | Let other programs read the game's exe path, so NVIDIA App keeps its per-game settings (0 = original lock) |
| `FpsLimit` | 0 | Frame rate limit, e.g. 144 for a 144 Hz monitor (0 = the game's usual 64 fps). The game logic is not touched, see below |
| `Width` / `Height` | — | Force an aspect if the one from the video settings is wrong |
| `DotAOriginal` | 1 | DracoL1ch's DotA Allstars maps run as without the mod (use their own WideScreen option), see below |
| `Debug` | 0 | Detailed log for bug reports |

## Troubleshooting

`W3TrueWidescreen.log` in the game folder says what the mod did:

- `unsupported game version`: this is not 1.26a.
- `WARNING: window is ...`: the window size does not match the resolution in the settings. Set `Width`/`Height` in the ini.
- `aspect is 4:3 or narrower`: nothing to do on a 4:3 screen.
- `movies: ...`: what the movie renderer did. If the movies misbehave, set `MovieRenderer=0` to fall back to the Windows renderer.
- `/fps` in the game chat shows less than `FpsLimit`: the game is hitting the monitor's refresh rate (vsync) or the GPU, not the limit. Set `FpsLimit` to the monitor's refresh rate.

## Compatibility notes

- Tested on 3840×2160 (16:9). Other aspects are computed the same way, but they have seen less testing.
- In-game cutscenes (rendered by the engine) keep the engine's look; only their letterbox is widened.
- The mod does not change gameplay or network data, but use it at your own discretion online.
- **DotA Allstars by DracoL1ch (7.0x)** builds its own interface with helper DLLs and has its own `WideScreen` option (in the map's settings) for the 3D view. With `DotAOriginal=1` the mod steps aside while such a map runs (menus, other maps and movies are unaffected) and only stretches the map's loading screen picture. With `DotAOriginal=0` the mod also works on these maps, but their interface is not adapted yet. These maps also hang when the game window is resized, with or without the mod.

## Building from source

The code is in `src/W3TrueWidescreen.c`. It builds with MinGW-w64 (i686):

```
i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -s -o W3TrueWidescreen.mix W3TrueWidescreen.c -lversion -lgdi32
```

## How it works (short)

The engine lays out its interface in a virtual 0.8 × 0.6 screen stretched to the monitor. The mod widens that space to `0.6 × aspect` and re-anchors the frames: the console to the centred 4:3 area, the menus to the edges. It then rebuilds the world and menu projection so the vertical field of view matches 4:3. The campaign fade model is read from the game archives, stretched to the screen width and cached in `W3TrueWidescreen_cache`. For the movies it skips the game's display mode switch and gamma ramp and, when the `W3TrueWidescreen` folder is present, builds the DirectShow graph from LAV Filters and MPC Video Renderer.

**Why the game runs at 64 fps.** The engine draws a frame whenever `GetTickCount()` returns a new value, and Windows updates that counter every 15.625 ms: 1000 / 15.625 = 64. Nothing in the game asks for 64, and its own `maxfps` setting changes nothing. `FpsLimit` gives the game a 1 ms version of that counter (from the performance counter), keeps the game tick on the real counter so the simulation, timers and everything stepped per tick run exactly as before, and raises the "draw a frame" flag on its own schedule, `1000 / FpsLimit` ms apart. Only rendering changes; a game hour still takes the same 20 real seconds.

## Changelog

- **1.6** — `DotAOriginal`: DracoL1ch's DotA Allstars maps run as without the mod (they bring their own interface and WideScreen option); their loading screen picture is stretched to the screen width.
- **1.5.1** — The movie renderer (MPC Video Renderer, LAV Filters) is loaded at the first movie instead of at startup. Loaded at startup, it made DracoL1ch's DotA Allstars 7.0x maps crash at hero selection (heap corruption).
- **1.5** — `FpsLimit`: a frame rate limit above the engine's 64 fps, e.g. 144 for a 144 Hz monitor (0 = unchanged). Game logic, timers and animations stepped per tick are untouched; see "How it works".
- **1.4.1** — Map clicks now work in the strip beside the top bar. With `WorldFullHeight=1` the world was drawn up to the top edge, but the game still rejected clicks above the original top of the world view (an upper limit on the cursor's y before picking the terrain); that limit is now lifted when the world is drawn full height.
- **1.4** — First public release.

## License

W3TrueWidescreen is MIT licensed, see `LICENSE`. Author: [Hr0ffT](https://github.com/Hr0ffT).

The optional `W3TrueWidescreen` folder contains unmodified third-party components under their own licenses (the texts are in the folder):

- [MPC Video Renderer](https://github.com/Aleksoid1978/VideoRenderer) 0.10.7 by Aleksoid1978, GPL-3.0. Source: https://github.com/Aleksoid1978/VideoRenderer/tree/0.10.7
- [LAV Filters](https://github.com/Nevcairiel/LAVFilters) 0.83 by Hendrik Leppkes, GPL-2.0, with FFmpeg libraries. Source: https://github.com/Nevcairiel/LAVFilters/tree/0.83

Warcraft III is a trademark of Blizzard Entertainment. This project is not affiliated with Blizzard.
