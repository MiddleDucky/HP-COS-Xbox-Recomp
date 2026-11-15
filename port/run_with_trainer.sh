#!/usr/bin/env bash
# The game and the debug trainer together, in one Wine prefix (one wineserver, so the trainer finds the game's
# shared block). The launcher by default; `run_with_trainer.sh direct` skips it and runs the game as run_win.sh
# does. The trainer closes when the game does.
here="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine}
export WINEDEBUG=${WINEDEBUG:--all}
before=$(pgrep -x hpcos_trainer.e | sort)
(cd "$here/build-win-trace" && exec wine ./hpcos_trainer.exe) &
sleep 2
# Wine re-parents its processes (names cut to 15 characters): note the trainer's own PIDs (not one started elsewhere) to close it later
mine=$(comm -13 <(echo "$before") <(pgrep -x hpcos_trainer.e | sort))
if [ "$1" = direct ]; then
    shift
    "$here/run_win.sh" "$@"
else
    "$here/run_launcher.sh" "$@"
    # the launcher starts hpcos.exe and may return before it ends: wait for the game itself
    sleep 3
    while pgrep -x hpcos.exe >/dev/null; do sleep 1; done
fi
[ -n "$mine" ] && kill $mine 2>/dev/null
exit 0
