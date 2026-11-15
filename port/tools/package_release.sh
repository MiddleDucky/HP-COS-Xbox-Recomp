#!/usr/bin/env bash
# The build players get: configures build-release (HPCOS_RELEASE=ON: stack canaries on the hand-written code,
# SSE4.1), builds the game and the launcher, and puts in dist/hpcos-<date>/:
#   hpcos_launcher.exe                    setup: 4:3 / 16:9, graphics, controls; unpacks a disc image; starts the game
#   bin\hpcos.exe                         the game: loads the DLL for the edition in game\
#   bin\hpcos_ntsc.dll, hpcos_pal.dll     the game itself, NTSC-US / PAL (whichever built)
#   bin\amd_fidelityfx_*.dll              AMD FSR 3.1 frame generation (loaded at run time)
#   bin\shadercache\base_shaders.bin      shaders compiled ahead (read only; the player's own go in shaders.bin)
#   bin\shadercache\base_pipelines.bin    the pipelines a play-through used, made ahead at the first start
#   README.txt, CHANGES.txt           for players
#   licenses\                         the port's, the toolkit's and the third-party licences
# VERSION=0.1.0 tools/package_release.sh also writes dist/hpcos_v<VERSION>_windows.zip (the folder's contents at
# its top). Players put their game\ folder beside the launcher; saves\ is made there on first run, and bin\hpcos.toml,
# bin\logs\ and the rest of bin\shadercache\ in bin\. All the program is in bin\ but the launcher. Symbols stay in
# the DLLs so crash reports name guest functions.
set -euo pipefail
port="$(cd "$(dirname "$0")/.." && pwd)"
cd "$port"
toolkit="${XBOXRECOMP_DIR:-$port/../xboxrecomp}"
cross=(-DCMAKE_TOOLCHAIN_FILE="$toolkit/cmake/mingw-w64-x86_64.cmake")
objdump=x86_64-w64-mingw32-objdump
nm=x86_64-w64-mingw32-nm
mkdir -p logs
cmake -S . -B build-release "${cross[@]}" -DCMAKE_BUILD_TYPE=Release -DHPCOS_RELEASE=ON \
      -DHPCOS_VERSION="${VERSION:-0.4.0}" > logs/build-release-config.log
nice -n 10 cmake --build build-release -j"$(nproc)" --target hpcos hpcos_launcher > logs/build-release.log 2>&1 ||
    { tail -30 logs/build-release.log; exit 1; }

dist="$port/dist/hpcos-$(date +%Y%m%d)"
# Never delete the folder: players (and testers) drop game/, settings and saves into it. Only the files below are
# replaced; anything else found there is left alone.
mkdir -p "$dist/licenses" "$dist/bin/shadercache"
cp -f build-release/hpcos_launcher.exe "$dist/"
cp -f build-release/bin/hpcos.exe build-release/bin/hpcos_ntsc.dll "$dist/bin/"
[ -f build-release/bin/hpcos_pal.dll ] && cp -f build-release/bin/hpcos_pal.dll "$dist/bin/"
cp -f build-release/bin/amd_fidelityfx_dx12.dll build-release/bin/amd_fidelityfx_loader_dx12.dll \
      build-release/bin/amd_fidelityfx_framegeneration_dx12.dll "$dist/bin/"
cp -f README.txt "$dist/"
cp -f CHANGES.md "$dist/CHANGES.txt"
# licences: the port's own (MIT), the toolkit's (MIT) and its attribution, the LGPL code from xemu linked into
# the game DLLs (src/apu, src/nv2a/nv2a_regs.h; the LGPL needs its text with every binary, README.txt points to the
# source), and the MIT code built in: the renderer and Dear ImGui
cp -f LICENSE "$dist/licenses/LICENSE.txt"
cp -f NOTICE "$dist/licenses/NOTICE.txt"
cp -f "$toolkit/LICENSE" "$dist/licenses/xboxrecomp-MIT.txt"
cp -f "$toolkit/NOTICE" "$dist/licenses/xboxrecomp-NOTICE.txt"
cp -f "$toolkit/LICENSES/LGPL-2.1.txt" "$dist/licenses/LGPL-2.1.txt"
cp -f src/renderer/LICENSE "$dist/licenses/renderer-MIT.txt"
cp -f third_party/imgui/LICENSE.txt "$dist/licenses/DearImGui.txt"
# AMD's FidelityFX DLLs: their binary licence
cp -f third_party/fidelityfx/LICENSE.md "$dist/licenses/AMD-FidelityFX.txt"
# the launcher title's font, Magic School One (FontMesa): its own terms, from its download
cp -f src/launcher/font/MagicSchool-licensing.txt "$dist/licenses/MagicSchool-font.txt"

# res/base_shaders.bin and res/base_pipelines.bin, from a play-through on Windows (its shadercache\shaders.bin and
# pipelines_list.bin): a player's first visit to an area then needs no shader compiler and its pipelines are made
# ahead at the first start; one their driver refuses is made on their PC instead
for f in base_shaders.bin base_pipelines.bin; do
    if [ -s "res/$f" ]; then cp -f "res/$f" "$dist/bin/shadercache/$f"
    else echo "warning: res/$f missing, not shipped" >&2; fi
done

# the security bits every player build must carry (ours: AMD's DLLs come signed as they are)
for exe in "$dist"/*.exe "$dist"/bin/*.exe "$dist"/bin/hpcos_*.dll; do
    bits=$("$objdump" -p "$exe" | grep -A4 DllCharacteristics)
    for want in HIGH_ENTROPY_VA DYNAMIC_BASE NX_COMPAT; do
        grep -q "$want" <<< "$bits" || { echo "$exe: $want missing" >&2; exit 1; }
    done
done
for dll in "$dist"/bin/hpcos_*.dll; do
    [ "$("$nm" "$dll" | grep -c __stack_chk_fail)" -gt 0 ] || { echo "$dll: no stack protector" >&2; exit 1; }
done

# every DLL the exes and the game DLLs import must come with Windows 10/11 itself (the C/C++ runtimes are linked in
# statically)
builtin='^(api-ms-win-crt-.*|kernel32|user32|gdi32|shell32|ole32|winmm|dwmapi|dbghelp|d3d11|d3d12|dxgi|d3dcompiler_47|xaudio2_8|xinput1_4|mfplat|mfreadwrite|windowscodecs|imm32|comctl32|advapi32|ntdll|bcrypt)\.dll$'
for exe in "$dist"/*.exe "$dist"/bin/*.exe "$dist"/bin/hpcos_*.dll; do
    "$objdump" -p "$exe" | sed -n 's/^\s*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u | while read -r dll; do
        grep -Eq "$builtin" <<< "$dll" || { echo "$(basename "$exe") needs $dll, which Windows does not ship" >&2; exit 1; }
    done
done

ls -la "$dist"

if [ -n "${VERSION:-}" ]; then
    zip="$port/dist/hpcos_v${VERSION}_windows.zip"
    rm -f "$zip"
    python3 - "$dist" "$zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, src))
PY
    ls -la "$zip"
fi
