# Harry Potter and the Chamber of Secrets (PAL/NTSC) — Xbox PC Recomp

A native Windows port of the original Xbox game *Harry Potter and the Chamber of Secrets* (2002), made by
**static recompilation**: the game's x86 code is translated to C and compiled for Windows, with the Xbox kernel,
Direct3D 8, DirectSound and input replaced by native code. No emulator.
**
Please note: This is not by any means completed work. I'm still actively working on this and do appreciate any feedback that will make this project better.
As such, while the games are playing from start to finish with no crashes (hopefully) some issues may arise (missing lighting, 4:3 buttons instead of 16:9 etc.). 
If you do notice something that doesn't look right, please raise it as an issue and I'll be more than happy to fix it.**

Features beyond the original:
- **Picture:** resolutions up to 4K with MSAA/SMAA and anisotropic filtering, and true 16:9 (widened 3D, with 2D
  and cutscene shots kept in shape).
- **Display:** windowed, borderless or fullscreen.
- **Sound:** 5.1 surround output.
- **Controls:** keyboard and mouse with rebinding and a movement pace on the mouse wheel; controller remapping,
  deadzones and vibration strength.
- **Accessibility:** colour-blind correction filters.
- **Smooth play:** shaders prepared ahead.

**This repository contains no game content.** To play or build it you need the files from your own copy of the
Xbox disc. The build regenerates the recompiled code from your `default.xbe`.

## Which disc

**Both PAL and NTSC versions of the game work**

## Playing

Download the latest release, put your NTSC-U disc's files or the .xiso in a `game` folder beside the executable and
start `hpcos_launcher.exe`. Requirements, controls and every option are in [port/README.txt](port/README.txt);
what changed in each release is in [port/CHANGES.md](port/CHANGES.md).

## Building

See [port/DEVELOPING.md](port/DEVELOPING.md): setup, regenerating the code from the XBE, building and making a
release.

## Credits and licences

The port's own code is MIT licensed, © 2026 MiddleDucky ([LICENSE](LICENSE)). The parts below keep their own
licences and copyright.

* **xboxrecomp** — the static recompilation toolkit this port is built on (runtime, kernel layer, pipeline),
  MIT, by sp00nz. Its own readme: [README.xboxrecomp.md](README.xboxrecomp.md); licence
  [LICENSES/xboxrecomp-MIT.txt](LICENSES/xboxrecomp-MIT.txt); third-party parts (including LGPL code from xemu) in
  [NOTICE](NOTICE) and [LICENSES/](LICENSES/).
* **The Direct3D 12 renderer** by TGP482 and Joshhhuaaa — the D3D8-to-Direct3D 12 renderer the port's graphics
  are based on, MIT ([port/src/renderer/LICENSE](port/src/renderer/LICENSE), [port/NOTICE](port/NOTICE)).
* **Dear ImGui** (MIT, port/third_party/imgui) and **SMAA** (MIT, Jorge Jimenez et al.).

### Thanks

These projects were references for hardware behaviour, XDK details or fixes
(no code taken unless listed above):

* [xemu](https://github.com/xemu-project/xemu) — Xbox hardware behaviour; the APU code listed in NOTICE
* [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) — XDK function addresses
* [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) and [xboxdevwiki](https://xboxdevwiki.net) — kernel and hardware reference
* [DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp) — single-core guest thread pinning
* [Unicorn Engine](https://github.com/unicorn-engine/unicorn) — the debug-only difftest build

*Harry Potter* and all related names and artwork are trademarks of and © Warner Bros. Entertainment Inc. The game
is © Electronic Arts. This is an unofficial fan project, not affiliated with or endorsed by Warner Bros.,
Electronic Arts or Microsoft.

This project has been aided by Claude Code.
