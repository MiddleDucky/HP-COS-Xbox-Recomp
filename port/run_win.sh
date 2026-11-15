#!/usr/bin/env bash
# Run the Windows build of hpcos under Wine from build-win-trace/ (the exes, game/, hpcos.toml,
# shadercache/). Wine hands these variables to the Windows process.
#   RECOMP_TITLE_KEVENTS  events the title builds itself (D3D, worker threads)
#   RECOMP_VBLANK         60 Hz vblank into the title's NV2A ISR
#   RECOMP_AC97_READY     codec ready + emulated APU behind trapped MMIO
#   RECOMP_DSP_ACK        DirectSound's DSP command word; only used with
#                         RECOMP_HLE_DSOUND=0 (XDK DirectSound on the emulated APU):
#                         hpcos.exe drops it otherwise (it zeroed WB movie data)
#   RECOMP_HLE_DSOUND     default on: DirectSound on a host XAudio2 mixer
#   RECOMP_PB_EXEC / RECOMP_FB_WINDOW  execute the pushbuffer, show the framebuffer
#   RECOMP_USB            emulated USB host + Controller S, fed by the PC's pad
#                         through XInput (an Xbox One/360 pad: LB = White, RB = Black)
# Graphics settings (display mode, resolution, internal resolution, MSAA/SMAA,
# filtering, brightness) live in hpcos.toml beside
# the exe and in the F10 panel in game; Alt+Enter flips windowed/borderless.
# A RECOMP_* variable set here wins over the file for that run, e.g.
#   RECOMP_WINDOW_SIZE=1920x1080 RECOMP_ASPECT=16:9 RECOMP_DISPLAY_MODE=borderless
#   RECOMP_INTERNAL_HEIGHT=1440 RECOMP_MSAA=4 RECOMP_SMAA=1 RECOMP_ANISO=16
#   RECOMP_HLE_D3D=0      old renderer: the emulated NV2A, drawn on the CPU
cd "$(dirname "$0")/build-win-trace" || exit 1
export RECOMP_TITLE_KEVENTS=1 RECOMP_VBLANK=1 RECOMP_AC97_READY=1
export RECOMP_DSP_ACK=0x834D0810
export RECOMP_PB_EXEC=1 RECOMP_FB_WINDOW=1
# the register mirror thread rests 250 us between passes instead of spinning a core
export RECOMP_ACK_SLEEP_US=${RECOMP_ACK_SLEEP_US:-250}
export RECOMP_USB=1
# hpcos.exe runs only from the launcher (src/single_instance.h); this script stands in for it
export HPCOS_LAUNCHER=1
export WINEDEBUG=${WINEDEBUG:--all}
# the GPU driver's compiled pipelines, kept per game: Wine's vkd3d has no pipeline library, so Mesa's disk cache is
# what carries them between runs, and its shared 1 GB default evicted them for other games
export MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-$PWD/shadercache/mesa}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
exec wine ./${HPCOS_EXE:-hpcos.exe} "$@"
