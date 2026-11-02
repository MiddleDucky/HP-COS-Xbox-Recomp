#!/usr/bin/env bash
# The build players get: configures build-release (HPCOS_RELEASE=ON: stack canaries on the hand-written code,
# SSE4.1), builds the game and the launcher, and puts in dist/hpcos-<date>/:
#   hpcos_launcher.exe                setup: 4:3 / 16:9, graphics, controls; unpacks a disc image; starts the game
#   hpcos.exe                         the game
#   shadercache\base_shaders.bin      shaders compiled ahead (read only; the player's own go in shaders.bin)
#   shadercache\base_pipelines.bin    the pipelines a play-through used, made ahead at the first start
#   README.txt, CHANGES.txt           for players
#   licenses\                         the port's, the toolkit's and the third-party licences
# VERSION=0.1.0 tools/package_release.sh also writes dist/hpcos_v<VERSION>_windows.zip (the folder's contents at
# its top). Players put the folder's contents beside their game/ folder; hpcos.toml, saves\, logs\ and the rest of
# shadercache\ are created on first run. Symbols stay in hpcos.exe so crash reports name guest functions.
set -euo pipefail
port="$(cd "$(dirname "$0")/.." && pwd)"
cd "$port"
toolkit="${XBOXRECOMP_DIR:-$port/../xboxrecomp}"
cross=(-DCMAKE_TOOLCHAIN_FILE="$toolkit/cmake/mingw-w64-x86_64.cmake")
objdump=x86_64-w64-mingw32-objdump
nm=x86_64-w64-mingw32-nm
mkdir -p logs
cmake -S . -B build-release "${cross[@]}" -DCMAKE_BUILD_TYPE=Release -DHPCOS_RELEASE=ON \
      -DHPCOS_VERSION="${VERSION:-0.3.0}" > logs/build-release-config.log
nice -n 10 cmake --build build-release -j"$(nproc)" --target hpcos hpcos_launcher > logs/build-release.log 2>&1 ||
    { tail -30 logs/build-release.log; exit 1; }

dist="$port/dist/hpcos-$(date +%Y%m%d)"
# Never delete the folder: players (and testers) drop game/, settings and saves into it. Only the files below are
# replaced; anything else found there is left alone.
mkdir -p "$dist/licenses" "$dist/shadercache"
cp -f build-release/hpcos.exe build-release/hpcos_launcher.exe "$dist/"
cp -f README.txt "$dist/"
cp -f CHANGES.md "$dist/CHANGES.txt"
# licences: the port's own (MIT), the toolkit's (MIT) and its attribution, the LGPL code from xemu linked into
# hpcos.exe (src/apu, src/nv2a/nv2a_regs.h; the LGPL needs its text with every binary, README.txt points to the
# source), and the MIT code built in: the renderer and Dear ImGui
cp -f LICENSE "$dist/licenses/LICENSE.txt"
cp -f NOTICE "$dist/licenses/NOTICE.txt"
cp -f "$toolkit/LICENSE" "$dist/licenses/xboxrecomp-MIT.txt"
cp -f "$toolkit/NOTICE" "$dist/licenses/xboxrecomp-NOTICE.txt"
cp -f "$toolkit/LICENSES/LGPL-2.1.txt" "$dist/licenses/LGPL-2.1.txt"
cp -f src/renderer/LICENSE "$dist/licenses/renderer-MIT.txt"
cp -f third_party/imgui/LICENSE.txt "$dist/licenses/DearImGui.txt"
# the launcher title's font, Magic School One (FontMesa): its own terms, from its download
cp -f src/launcher/font/MagicSchool-licensing.txt "$dist/licenses/MagicSchool-font.txt"

# res/base_shaders.bin and res/base_pipelines.bin, from a play-through on Windows (its shadercache\shaders.bin and
# pipelines_list.bin): a player's first visit to an area then needs no shader compiler and its pipelines are made
# ahead at the first start; one their driver refuses is made on their PC instead
for f in base_shaders.bin base_pipelines.bin; do
    if [ -s "res/$f" ]; then cp -f "res/$f" "$dist/shadercache/$f"
    else echo "warning: res/$f missing, not shipped" >&2; fi
done

# the security bits every player build must carry
for exe in "$dist"/*.exe; do
    bits=$("$objdump" -p "$exe" | grep -A4 DllCharacteristics)
    for want in HIGH_ENTROPY_VA DYNAMIC_BASE NX_COMPAT; do
        grep -q "$want" <<< "$bits" || { echo "$exe: $want missing" >&2; exit 1; }
    done
done
[ "$("$nm" "$dist/hpcos.exe" | grep -c __stack_chk_fail)" -gt 0 ] || { echo "hpcos.exe: no stack protector" >&2; exit 1; }

# every DLL either exe imports must come with Windows 10/11 itself (the C/C++ runtimes are linked in statically)
builtin='^(api-ms-win-crt-.*|kernel32|user32|gdi32|shell32|ole32|winmm|dwmapi|dbghelp|d3d11|d3d12|dxgi|d3dcompiler_47|xaudio2_8|xinput1_4|mfplat|mfreadwrite|windowscodecs|imm32|comctl32|advapi32|ntdll|bcrypt)\.dll$'
for exe in "$dist"/*.exe; do
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
