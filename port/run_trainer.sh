#!/usr/bin/env bash
# The debug trainer (hpcos_trainer.exe) under Wine, beside a running hpcos.exe (start either first): noclip,
# infinite health, every spell, cards, beans, level warps, saved positions. Same Wine prefix as the game, so they
# find each other.
cd "$(dirname "$0")/build-win-trace" || exit 1
export WINEDEBUG=${WINEDEBUG:--all}
exec wine ./hpcos_trainer.exe "$@"
