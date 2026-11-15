/* The game edition a build is for. One source tree builds both: hpcos_ntsc.dll from the NTSC-US Chamber of Secrets and,
 * with HPCOS_PAL=1 (CMakeLists.txt's hpcos_pal target), hpcos_pal.dll from the PAL one (Europe: En, Fr, De, Es, It).
 * The two XBEs are separate links of the same source (the same title ID, file size and sections; PAL's code is 0x260
 * bytes longer); the hand-written code keeps the NTSC addresses and the PAL target compiles copies translated by
 * tools/edition/ntsc2pal.py. Only what really differs between the discs is under HPCOS_PAL: the edition check's
 * values and names, and which English the game picks by itself. */
#ifndef HPCOS_EDITION_H
#define HPCOS_EDITION_H

#ifndef HPCOS_PAL
#define HPCOS_PAL 0
#endif

#if HPCOS_PAL
#define HPCOS_EDITION_NAME "PAL (Europe)"
#define HPCOS_XBE_ENTRY 0x0010E8F8u
#define HPCOS_XBE_TEXT_SIZE 1177728u
#else
#define HPCOS_EDITION_NAME "NTSC-US (USA)"
#define HPCOS_XBE_ENTRY 0x0010E698u
#define HPCOS_XBE_TEXT_SIZE 1177120u
#endif

#endif
