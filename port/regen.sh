#!/usr/bin/env bash
# Disassemble + recompile an edition's default.xbe. Every hand-written override file is listed with --exclude-manual,
# so the function it replaces keeps the generated body as sub_X_gen.
#
#   ./regen.sh          NTSC (the edition the port is written against)  -> src/recomp/gen      (hpcos.exe)
#   ./regen.sh pal      PAL (Europe, En/Fr/De/Es/It)                     -> src/recomp/gen_pal  (hpcos_pal.exe)
#
# The XBEs are found by their code size (NTSC .text 1177120 bytes, PAL 1177728), first match of:
#   NTSC: $XBE, <toolkit>/game/default.xbe, <toolkit>/game/ntsc/default.xbe, build-win-trace/game/ntsc/default.xbe,
#         build-win-trace/game/default.xbe
#   PAL:  $XBE_PAL, <toolkit>/game/pal/default.xbe, <toolkit>/game/default.xbe, build-win-trace/game/default.xbe,
#         build-win-trace/game/pal/default.xbe
#
# The hand-written code is written with NTSC addresses. For PAL the override files, the icall seeds and patch_gen.py
# are first translated by tools/edition/ntsc2pal.py (its map is remade from both XBEs and NTSC's function list), the
# same translation the build compiles hpcos_pal.exe from; PAL's pipeline files go to tools/edition/work_pal, so the
# NTSC ones in the toolkit stay as they are.
#
# Works with the toolkit around this folder (the GitHub repository: the toolkit at its top, the port in port/) or
# beside it (../xboxrecomp, a working copy). Needs Python 3 (and capstone for pal).
set -euo pipefail
port="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$port/../xboxrecomp/tools/recomp" ]; then toolkit="$(cd "$port/../xboxrecomp" && pwd)"
else toolkit="$(cd "$port/.." && pwd)"; fi
EDITION=${1:-ntsc}
MANUAL=(src/hle_crc.c src/hle_debug.c src/hle_dsound.c src/hle_fade.c src/hle_frontend.c src/hle_gringotts.c
        src/hle_hud.c src/hle_input.c src/hle_rain.c src/hle_shots.c src/hle_switch.c src/hle_trainer.c src/hle_video.c
        src/renderer/xdk/hle_d3d8.cpp)
E="$port/tools/edition"
SECTIONS=D3D,D3DX,XGRPH,DSOUND,XMV,XPP

# the .text size of an XBE (empty if the file is not there)
text_size() { [ -f "$1" ] && python3 -c "
import struct,sys
d=open(sys.argv[1],'rb').read(0x10000); b=struct.unpack_from('<I',d,0x104)[0]; s=struct.unpack_from('<I',d,0x120)[0]-b
print(struct.unpack_from('<I',d,s+8)[0])" "$1" || true; }
find_xbe() {  # $1 = wanted .text size, then candidates
    local want=$1; shift
    for c in "$@"; do
        [ -n "$c" ] && [ "$(text_size "$c")" = "$want" ] && { echo "$(cd "$(dirname "$c")" && pwd)/$(basename "$c")"; return; }
    done
}
NTSC_XBE=$(find_xbe 1177120 "${XBE:-}" "$toolkit/game/default.xbe" "$toolkit/game/ntsc/default.xbe" \
           "$port/build-win-trace/game/ntsc/default.xbe" "$port/build-win-trace/game/default.xbe")
PAL_XBE=$(find_xbe 1177728 "${XBE_PAL:-}" "$toolkit/game/pal/default.xbe" "$toolkit/game/default.xbe" \
          "$port/build-win-trace/game/default.xbe" "$port/build-win-trace/game/pal/default.xbe")
[ -n "$NTSC_XBE" ] || { echo "no NTSC default.xbe found (set XBE=)" >&2; exit 1; }
cd "$toolkit"

case "$EDITION" in
ntsc)
    work="$(dirname "$NTSC_XBE")"
    # the XBE's layout (sections, imports, entry), which disasm reads beside the XBE as default_analysis.json
    python3 -m tools.xbe_parser "$NTSC_XBE" --json "$work/default_analysis.json"
    # icall_seeds.json: functions only reached through pointers (vtables, callbacks) the scan would not find
    python3 -m tools.disasm "$NTSC_XBE" --text-only --extra-sections $SECTIONS --seed-functions "$port/icall_seeds.json"
    python3 -m tools.func_id "$NTSC_XBE"
    python3 -m tools.abi_analysis "$NTSC_XBE"
    EXCLUDE=()
    for f in "${MANUAL[@]}"; do EXCLUDE+=(--exclude-manual "$port/$f"); done
    python3 -m tools.recomp "$NTSC_XBE" --all --split 1000 --gen-dir "$port/src/recomp/gen" --game-name hpcos "${EXCLUDE[@]}"
    # fixes inside generated functions the lifter gets wrong (tools/patch_gen.py: fails if one no longer applies)
    python3 "$port/tools/patch_gen.py" "$port/src/recomp/gen"
    ;;
pal)
    [ -n "$PAL_XBE" ] || { echo "no PAL default.xbe found (set XBE_PAL=)" >&2; exit 1; }
    echo "NTSC: $NTSC_XBE"
    echo "PAL:  $PAL_XBE"
    wn="$E/work_ntsc" wp="$E/work_pal" src="$port/build-pal-src"
    mkdir -p "$wn" "$wp"
    # NTSC's functions as the seeds find them today (into work_ntsc, not the toolkit's output): the map's input
    python3 -m tools.xbe_parser "$NTSC_XBE" --json "$wn/default_analysis.json" >/dev/null
    python3 -m tools.disasm "$NTSC_XBE" --text-only --extra-sections $SECTIONS --analysis-json "$wn/default_analysis.json" \
        --seed-functions "$port/icall_seeds.json" -o "$wn"
    cp "$wn/functions.json" "$E/ntsc_functions.json"
    python3 "$E/ntsc2pal.py" map "$NTSC_XBE" "$PAL_XBE" "$E/ntsc_functions.json" -o "$E/ntsc2pal_map.json"
    # the overrides, seeds and generated-code fixes with PAL addresses (fails on anything rules.txt does not decide)
    python3 "$E/ntsc2pal.py" translate "$E/ntsc2pal_map.json" --ntsc "$NTSC_XBE" --pal "$PAL_XBE" --rules "$E/rules.txt" --strict --quiet \
        --root "$port" --out-dir "$src" "${MANUAL[@]/#/$port/}" "$port/icall_seeds.json" "$port/tools/patch_gen.py"
    python3 -m tools.xbe_parser "$PAL_XBE" --json "$wp/default_analysis.json"
    python3 -m tools.disasm "$PAL_XBE" --text-only --extra-sections $SECTIONS --analysis-json "$wp/default_analysis.json" \
        --seed-functions "$src/icall_seeds.json" -o "$wp"
    python3 -m tools.func_id "$PAL_XBE" --functions "$wp/functions.json" --strings "$wp/strings.json" \
        --xrefs "$wp/xrefs.json" -o "$wp/func_id"
    python3 -m tools.abi_analysis "$PAL_XBE" --disasm-dir "$wp" --func-id-dir "$wp/func_id" --output-dir "$wp/abi"
    EXCLUDE=()
    for f in "${MANUAL[@]}"; do EXCLUDE+=(--exclude-manual "$src/$f"); done
    python3 -m tools.recomp "$PAL_XBE" --all --split 1000 --gen-dir "$port/src/recomp/gen_pal" --game-name hpcos \
        --disasm-dir "$wp" --func-id-dir "$wp/func_id" --abi-dir "$wp/abi" "${EXCLUDE[@]}"
    python3 "$src/tools/patch_gen.py" "$port/src/recomp/gen_pal"
    ;;
*)
    echo "usage: ./regen.sh [ntsc|pal]" >&2; exit 1 ;;
esac
