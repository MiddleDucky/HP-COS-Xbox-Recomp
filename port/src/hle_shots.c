/**
 * Cutscene shots framed for true 16:9 (aspect '16:9' in F10 / hpcos.toml).
 *
 * In 16:9 the camera sees wider than the Xbox's 4:3, and a few cutscene shots then show what 4:3 kept out of frame
 * (a character at the side moving oddly). For those shots the lens is narrowed so the width seen is the Xbox's 4:3
 * width: the shot is zoomed in, the top and bottom lose a little, and the sides show what the Xbox showed.
 *
 * sub_00011100 builds the camera's projection from the aspect [0x50C2B8] / [0x50C2BC], the half field of view
 * [0x50C1DC] and near / far [0x50C1E4] / [0x50C1E8]; the eye is at [0x50C1F0]. A shot is told by its level, its lens
 * and where its (still) camera stands. RECOMP_SHOT_ZOOM=0 = the wide 16:9 view for every shot.
 *
 * Built into the recompile with --exclude-manual ../hpcos/src/hle_shots.c; gen/recomp_0000.c's generated body is
 * renamed sub_00011100_gen by hand until then.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

extern void sub_00011100_gen(void);
extern int hpcos_Widescreen(void);  /* renderer/xdk/hle_d3d8.cpp: booted in 16:9 */

#define LEVEL_INDEX 0x527B78u
#define ASPECT_NUM  0x50C2B8u
#define ASPECT_DEN  0x50C2BCu
#define HALF_FOV    0x50C1DCu
#define CAMERA_EYE  0x50C1F0u

typedef struct Shot {
    const char *name;
    uint32_t level;
    float half_fov;
    float eye[3];
} Shot;

static const Shot kShots[] = {
    /* Gryffindor Common Room, Night 5: Harry tells Ron the monster is a basilisk; Ron at the right edge, out of the
     * Xbox's frame, moves oddly in 16:9 */
    {"common room, Harry and Ron (basilisk)", 30, 0.2618f, {6.4286f, 1.1134f, 1.046f}},
    /* Hogwarts Exterior after the Quidditch match: on Hermione, Ron at the left edge, out of the Xbox's frame
     * (2026-10-24, F10 MARK) */
    {"hogwarts exterior, Hermione (after Quidditch)", 26, 0.2618f, {-150.3343f, 25.2672f, 184.7593f}},
};

static const Shot *current_shot(void)
{
    for (size_t i = 0; i < sizeof kShots / sizeof kShots[0]; i++) {
        const Shot *s = &kShots[i];
        if (MEM32(LEVEL_INDEX) != s->level || fabsf(MEMF(HALF_FOV) - s->half_fov) > 0.001f) continue;
        float d = 0;
        for (int j = 0; j < 3; j++) d += fabsf(MEMF(CAMERA_EYE + 4u * j) - s->eye[j]);
        if (d < 0.05f) return s;
    }
    return NULL;
}

/* Field of view (F10 Graphics, % of the game's). The half angle at [0x50C1DC] (radians) is read by the projection
 * builders (sub_00011100 and its second entry sub_00011106) and by the culling (sub_00024E00, sub_000254F0,
 * sub_00025E10, sub_00026830, sub_00027090, sub_000274D0: what lies outside the view cone is not drawn), so all of them
 * see it widened, for their call only: the camera code keeps the game's angle. The percentage scales the half
 * angle's tangent, so 120 % shows 20 % more of the scene each way. This title has no cutscene test: the slider holds in
 * cutscenes too, apart from the shots kShots narrows */
uint32_t hpcos_FovPercent(void);  /* renderer/app/app.cpp: 80 to 130, 100 as the game */

int hpcos_InGameView(void);       /* hle_video.c: Harry's update running, no 4:3 screen up (the inventory keeps the game's lens) */

static void fov_call(void (*gen)(void))
{
    uint32_t pct = hpcos_InGameView() ? hpcos_FovPercent() : 100u;
    float half = MEMF(HALF_FOV);
    if (pct == 100 || !(half > 0.01f && half < 1.5f)) {
        gen();
        return;
    }
    MEMF(HALF_FOV) = atanf(tanf(half) * (float)pct / 100.0f);
    gen();
    MEMF(HALF_FOV) = half;
}

extern void sub_00011106_gen(void);
void sub_00011106(void) { fov_call(sub_00011106_gen); }
extern void sub_00024E00_gen(void);
void sub_00024E00(void) { fov_call(sub_00024E00_gen); }
extern void sub_000254F0_gen(void);
void sub_000254F0(void) { fov_call(sub_000254F0_gen); }
extern void sub_00025E10_gen(void);
void sub_00025E10(void) { fov_call(sub_00025E10_gen); }
extern void sub_00026830_gen(void);
void sub_00026830(void) { fov_call(sub_00026830_gen); }
extern void sub_00027090_gen(void);
void sub_00027090(void) { fov_call(sub_00027090_gen); }
extern void sub_000274D0_gen(void);
void sub_000274D0(void) { fov_call(sub_000274D0_gen); }

void sub_00011100(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_SHOT_ZOOM");
        enabled = !(e && e[0] == '0');
    }
    float aspect = MEMF(ASPECT_DEN) != 0 ? MEMF(ASPECT_NUM) / MEMF(ASPECT_DEN) : 0;
    const Shot *s = enabled && aspect > 1.5f && hpcos_Widescreen() ? current_shot() : NULL;
    if (!s) {
        fov_call(sub_00011100_gen);
        return;
    }
    /* the 4:3 width: tan(half fov') * aspect = tan(half fov) * 4/3 */
    float half_fov = MEMF(HALF_FOV);
    MEMF(HALF_FOV) = atanf(tanf(half_fov) * (4.0f / 3.0f) / aspect);
    sub_00011100_gen();
    MEMF(HALF_FOV) = half_fov;

    static const Shot *logged;
    if (logged != s) {
        logged = s;
        fprintf(stderr, "[SHOT] %s: zoomed to the 4:3 width\n", s->name);
    }
}

/* F10's MARK: the camera as kShots tells a shot, so a cutscene shot showing too much in 16:9 can be added */
void hpcos_ShotLog(void)
{
    fprintf(stderr, "[SHOT] level %u, half fov %.4f, eye {%.4ff, %.4ff, %.4ff}, aspect %.4f%s\n", MEM32(LEVEL_INDEX),
            MEMF(HALF_FOV), MEMF(CAMERA_EYE), MEMF(CAMERA_EYE + 4), MEMF(CAMERA_EYE + 8),
            MEMF(ASPECT_DEN) != 0 ? MEMF(ASPECT_NUM) / MEMF(ASPECT_DEN) : 0.0f, current_shot() ? " (a zoomed shot)" : "");
}

/* the renderer's cutscene test (d3d12.cpp: depth of field in cutscenes). This title has none yet, so the answer is
 * no and the effect stays off; its setting is not offered in F10 */
int hpcos_Cutscene(void) { return 0; }

/* debug: who opens a level's pieces. The kernel's path hook (xboxrecomp kernel_path.c) is set here; for a .geo file the
 * title's return addresses on the guest stack are logged, call sites only, to find the code that streams a level's
 * parts in as Harry moves (landing1.geo, landing2.geo on the staircases); the name is handed to hle_trainer.c, which
 * ends the flying lesson's arch with its last cutscene */
#include <string.h>
extern void (*g_xbox_path_hook)(const char *xbox_path);
static void geo_path_hook(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e || _stricmp(e, ".geo")) return;
    char line[512];
    int n = snprintf(line, sizeof line, "[GEO] open %s from", strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    int k = 0;
    /* the game's own code only (below the XDK's libraries from 0x10C000): the file system's own frames filled the
     * list before the caller that asked (2026-11-01: which code loads a spell's harryXXX.geo) */
    for (uint32_t i = 0; i < 4096 && k < 16 && n < (int)sizeof line - 12; i++) {
        uint32_t v = MEM32(g_esp + 4 * i);
        if (v < 0x11005 || v >= 0x10C000) continue;
        if (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF)
            n += snprintf(line + n, sizeof line - n, " %08X", v), k++;
    }
    fprintf(stderr, "%s\n", line);
    extern void hpcos_GeoOpened(const char *name);
    hpcos_GeoOpened(strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
}
__attribute__((constructor)) static void geo_path_hook_init(void) { g_xbox_path_hook = geo_path_hook; }
