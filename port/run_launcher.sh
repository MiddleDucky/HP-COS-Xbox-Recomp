#!/usr/bin/env bash
# The launcher (hpcos_launcher.exe) under Wine: 4:3 or 16:9 and the graphics settings, then the game with the
# environment run_win.sh sets. Both exes and hpcos.toml sit in build-win-trace/ beside game/.
cd "$(dirname "$0")/build-win-trace" || exit 1
export WINEDEBUG=${WINEDEBUG:--all}
# the GPU driver's compiled pipelines, kept per game: Wine's vkd3d has no pipeline library, so Mesa's disk cache is
# what carries them between runs, and its shared 1 GB default evicted them for other games
export MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-$PWD/shadercache/mesa}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
exec wine ./hpcos_launcher.exe "$@"
