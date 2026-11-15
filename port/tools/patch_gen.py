#!/usr/bin/env python3
"""Fixes regen.sh applies to the generated code after a regeneration (python3 tools/patch_gen.py src/recomp/gen).

Four functions start with a conditional jump on the flags of their CALLER's compare (the caller does `cmp` or
`test`, then jumps into them). The lifter gives each function a fresh `_flags = 0`, so the branch was never taken:
the Whomping Willow's right hand ended its solo strike on its first frame, and three other checks were skipped.
Until the toolkit's lifter learns this, each entry test is rewritten here to the caller's compare. Every fix finds
its line by exact text inside its own function and fails loudly if the line is gone (a lifter change), so a regen
never drops one silently. Running it twice changes nothing.
"""
import glob
import re
import sys

# function, the lifted line, the fixed line
FIXES = [
    ("sub_00018D40",  # the movie player (name, skippable): with skip_intro the boot's Warner Bros. and EA logo
                      # movies are not played (app.cpp hpcos_SkipMovie). cdecl and its return value unread by every
                      # caller (each pops its two arguments), so a plain ret is the movie having ended
     "loc_00018D40: ;\n",
     "loc_00018D40: ;\n    { extern int hpcos_SkipMovie(const char *name); if (hpcos_SkipMovie((const char *)XBOX_PTR(MEM32(esp + 4)))) { eax = 0; esp += 4; return; } } /* hand-fixed: F10 Enhancements, skip the logo movies */\n"),
    ("sub_00105D57",  # willow hand state 6 (the right hand's solo strike): waits for event 0x67
     "    if (_flags /* jne: not equal / not zero */) { g_seh_ebp = ebp; sub_00106547(); return; }",
     "    if (eax != 0x67u /* jne: flags of the caller's cmp eax, 0x67 (0x105F3A) - hand-fixed, the lifted _flags was always 0 */) { g_seh_ebp = ebp; sub_00106547(); return; }"),
    ("sub_00105F06",  # willow slam impact spawn: null check
     "    if (_flags /* je: equal / zero */) { g_seh_ebp = ebp; sub_00106547(); return; }",
     "    if (eax == 0 /* je: flags of the caller's cmp eax, 0 (0x1060DD) - hand-fixed, the lifted _flags was always 0 */) { g_seh_ebp = ebp; sub_00106547(); return; }"),
    ("sub_0009D928",
     "    if (_flags /* jne: not equal / not zero */) goto loc_0009D931;",
     "    if (eax != 2u /* jne: flags of the caller's cmp eax, 2 (0x9D79E) - hand-fixed, the lifted _flags was always 0 */) goto loc_0009D931;"),
    ("sub_000C0001",  # null check on [esi+0xB0]
     "    if (_flags /* je: equal / zero */) goto loc_000C000E;",
     "    if (eax == 0 /* je: flags of the caller's test eax, eax (0xBFFFF) - hand-fixed, the lifted _flags was always 0 */) goto loc_000C000E;"),
    ("sub_00048F60",  # the room draw: a room's list drawn only if this frame's visibility pass stamped it (+0x40 ==
                      # [level+0x68]+0x298); with draw_all_objects every room of the loaded level is drawn, its objects
                      # still culled one by one against the camera's view (the staircase's landings were left out from
                      # the floor below until Harry climbed into them)
     "    if (CMP_NE(_fa, _fb)) goto loc_00048FDD; /* jne: not equal / not zero */",
     "    { extern int hpcos_AllRooms(void); if (CMP_NE(_fa, _fb) && !hpcos_AllRooms()) goto loc_00048FDD; } /* jne - hand-fixed: every room of the loaded level drawn with draw_all_objects (hle_debug.c) */"),
    ("sub_00049EB0",  # the visibility pass: after the doorway walk (or the broom's room list), rooms flagged 2 that
                      # it did not reach are marked seen with the camera's whole view; with draw_all_objects every room
                      # of every loaded level is, as the broom's list does on the grounds, so the castle's rooms no
                      # longer come and go with where the camera looks (their objects still culled one by one)
     "    if (TEST_Z(_fa, _fb)) goto loc_0004A0DA; /* je: equal / zero */\n\nloc_0004A08F: ;",
     "    { extern int hpcos_AllRooms(void); if (TEST_Z(_fa, _fb) && !hpcos_AllRooms()) goto loc_0004A0DA; } /* je - hand-fixed: every room marked seen with draw_all_objects (hle_debug.c) */\n\nloc_0004A08F: ;"),
    ("sub_0002F260",  # the collision query: an object it ran into (its record in the per-object loop, [esp+0x18])
                      # handed to hle_trainer.c with the query, to find the grounds' border walls and pass over them
     "    ecx = MEM32(esp + 0x18);\n    MEM8(ebx + 0x34) = 1;",
     "    ecx = MEM32(esp + 0x18);\n    MEM8(ebx + 0x34) = 1;\n    { extern void hpcos_CollisionHit(uint32_t query, uint32_t obj, uint32_t strip); hpcos_CollisionHit(ebx, MEM32(esp + 0x10) - 0x228u, ecx); } /* hand-fixed: the hit reported (hle_trainer.c) */"),
    ("sub_000E6860",  # the grounds' script each frame: a warp zone Harry stands in (type 0x52000166, found by
                      # sub_0002D3E0) fades out and puts him at its destination ([zone+0x28], sub_000E64E0); with the
                      # path warps off the zone is passed over and he walks on, except the flying lesson's
                      # (hpcos_GroundsWarpAllowed: the lesson starts with its warp)
     "    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp edi, 0 (32-bit) */\n"
     "    if (CMP_EQ(_fa, _fb)) goto loc_000E69DE; /* je: equal / zero */",
     "    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp edi, 0 (32-bit) */\n"
     "    { extern int hpcos_GroundsWarpAllowed(uint32_t dest); if (CMP_EQ(_fa, _fb) || !hpcos_GroundsWarpAllowed(MEM32(edi + 0x28))) goto loc_000E69DE; } /* je - hand-fixed: the grounds' path warps can be turned off, the flying lesson's kept (hle_trainer.c) */"),
    ("sub_000E6860",  # the same script's prompt: a prompt zone Harry stands in (type 0x52000167, esi) shows where the
                      # path goes (text id [0x20AC00 + [zone+0x28]*4], sub_0003BAC0 / sub_000AEDD0); with the path warps
                      # off the paths go nowhere, so it is taken as no prompt zone and one still showing is cleared
                      # (loc_000E6969, sub_000AEAE0)
     "    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp esi, 0 (32-bit) */\n"
     "    if (CMP_EQ(_fa, _fb)) goto loc_000E6969; /* je: equal / zero */",
     "    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp esi, 0 (32-bit) */\n"
     "    { extern int hpcos_GroundsPromptOpen(uint32_t dest); if (CMP_EQ(_fa, _fb) || !hpcos_GroundsPromptOpen(MEM32(esi + 0x28))) goto loc_000E6969; } /* je - hand-fixed: no destination names for warps turned off (Explore Hogwarts Grounds) */"),
    ("sub_000AD2E0",  # the HUD's "is this action unavailable" (1 greys the button): the broom's (0x1004) is available
                      # only on the grounds, level 0x1A, which day and night share; at night, on foot (not [0x5302A0]
                      # bit 4, so landing stays as it is), it is now unavailable (hle_trainer.c, hpcos_NightGrounds)
     "    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(0x527B78), 0x1A (32-bit) */\n"
     "    if (CMP_NE(_fa, _fb)) goto loc_000AD35C; /* jne: not equal / not zero */",
     "    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(0x527B78), 0x1A (32-bit) */\n"
     "    { extern int hpcos_NightGrounds(void); if (CMP_NE(_fa, _fb) || (!(MEM8(0x5302A0) & 4) && hpcos_NightGrounds())) goto loc_000AD35C; } /* jne - hand-fixed: no broom at night (hle_trainer.c) */"),
    ("sub_000E6760",  # a grounds warp object's update: Harry coming within [obj+0x90] of its point (obj+0x80) warps
                      # him to [obj+0x98] (sub_000E64B0); with the path warps off it does not, except the flying
                      # practice door's (destination 2, the lesson: hpcos_GroundsWarpAllowed)
     "    PUSH32(esp, 0x000E67DAu); RECOMP_ABI_CALL(0x000E64B0u, sub_000E64B0); /* call 0x000E64B0 */",
     "    { extern int hpcos_GroundsWarpAllowed(uint32_t dest); if (hpcos_GroundsWarpAllowed(edx)) { PUSH32(esp, 0x000E67DAu); RECOMP_ABI_CALL(0x000E64B0u, sub_000E64B0); } } /* call 0x000E64B0 - hand-fixed: the path warps can be turned off, the flying lesson's kept (hle_trainer.c) */"),
    ("sub_000E6760",  # the same warp object's prompt: within its outer radius ([obj+0x94]) it shows the text
                      # [0x20AC10] (where it goes, e.g. back to the entrance from the stadium) until Harry leaves; with the
                      # path warps off it goes nowhere, so the prompt is not shown (2026-10-30), except the night field's exit
     "    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), LO8(ebx) (8-bit) */\n"
     "    if (CMP_NE(_fa, _fb)) goto loc_000E684F; /* jne: not equal / not zero */",
     "    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), LO8(ebx) (8-bit) */\n"
     "    { extern int hpcos_GroundsPromptOpen(uint32_t dest); if (CMP_NE(_fa, _fb) || !hpcos_GroundsPromptOpen(MEM32(esi + 0x98))) goto loc_000E684F; } /* jne - hand-fixed: no warp prompt for warps turned off (Explore Hogwarts Grounds) */"),
    ("sub_000E4470",  # the grounds' area switch: with the path warps off every area is the broom's (0xD), whose room
                      # list leaves out only room 0xFA, so the whole grounds is drawn and solid as when flying; the
                      # flying lesson's warp takes area 9, every room, as the lesson's own area 0xB shows room 0xFA.
                      # The night grounds (hogwarts_night.geo) have areas 0x10-0x15 over their own full list
                      # (0x20AA28); 0x10-0x14 each leave rooms out, 0x15 none (its list at 0x20AA24 is empty), so
                      # with the path warps off they become 0x15 and the whole night castle is there (2026-10-30)
     "    eax = MEM32(esp + 0x14);",
     "    { extern int hpcos_GroundsWarps(void); extern uint32_t hpcos_GroundsArea(void); if (!hpcos_GroundsWarps() && MEM32(esp + 0x14) < 0x10u) MEM32(esp + 0x14) = hpcos_GroundsArea(); else if (!hpcos_GroundsWarps() && MEM32(esp + 0x14) >= 0x10u && MEM32(esp + 0x14) < 0x15u) MEM32(esp + 0x14) = 0x15u; } eax = MEM32(esp + 0x14); /* hand-fixed: the broom's area with the path warps off, every room for the flying lesson (hle_trainer.c); at night area 0x15, which leaves out no room */"),
    ("sub_000E5E90",  # the level start's visible rooms, same; esi holds the full list
     '    MEM32(0x52794C) = 0x545D10;\n    if (CMP_EQ(_fa, _fb)) goto loc_000E5F26; /* je: equal / zero */',
     '    { extern int hpcos_GroundsWarps(void); MEM32(0x52794C) = hpcos_GroundsWarps() ? 0x545D10 : esi; } /* hand-fixed: with the path warps off every room of the full list (esi) drawn, as on the broom */\n    if (CMP_EQ(_fa, _fb)) goto loc_000E5F26; /* je: equal / zero */'),
    ("sub_000E5E90",  # and the second, same
     '    MEM32(0x52794C) = 0x545D10;\n    if (CMP_EQ(_fa, _fb)) goto loc_000E6026; /* je: equal / zero */',
     '    { extern int hpcos_GroundsWarps(void); MEM32(0x52794C) = hpcos_GroundsWarps() ? 0x545D10 : esi; } /* hand-fixed: with the path warps off every room of the full list (esi) drawn, as on the broom */\n    if (CMP_EQ(_fa, _fb)) goto loc_000E6026; /* je: equal / zero */'),
    ("sub_000E5E90",  # the grounds' area set at the level's start, same
     '    PUSH32(esp, 0x000E5F1Bu); RECOMP_ABI_CALL(0x000488A0u, sub_000488A0); /* call 0x000488A0 */',
     '    { extern int hpcos_GroundsWarps(void); PUSH32(esp, 0x000E5F1Bu); if (hpcos_GroundsWarps()) RECOMP_ABI_CALL(0x000488A0u, sub_000488A0); else RECOMP_ABI_CALL(0x000488E0u, sub_000488E0); } /* call 0x000488A0 - hand-fixed: with the path warps off the piece is shown instead (sub_000488E0, same arguments) */'),
    ("sub_000E5E90",  # and its second list, same
     '    PUSH32(esp, 0x000E601Bu); RECOMP_ABI_CALL(0x000488A0u, sub_000488A0); /* call 0x000488A0 */',
     '    { extern int hpcos_GroundsWarps(void); PUSH32(esp, 0x000E601Bu); if (hpcos_GroundsWarps()) RECOMP_ABI_CALL(0x000488A0u, sub_000488A0); else RECOMP_ABI_CALL(0x000488E0u, sub_000488E0); } /* call 0x000488A0 - hand-fixed: with the path warps off the piece is shown instead (sub_000488E0, same arguments) */'),
    ("sub_00050300",  # the broom's landing: Harry in a landing zone (the zone flags under him, [0x53030C], bit
                      # 0x01000000) pressing the land button (action 0x1004) dismounts (state 0x76, dismount_broom);
                      # with F10's broom option a flat surface up to 3 under him is a landing zone (hpcos_BroomLandHere)
     "    PUSH32(esp, edi);\n    if (TEST_Z(_fa, _fb)) goto loc_00050345; /* je: equal / zero */\n\nloc_0005030F: ;",
     "    PUSH32(esp, edi);\n    { extern int hpcos_BroomLandHere(void); if (TEST_Z(_fa, _fb) && !hpcos_BroomLandHere()) goto loc_00050345; } /* je - hand-fixed: with F10's broom option a flat surface under Harry is a landing zone (hle_trainer.c) */\n\nloc_0005030F: ;"),
]


def main(gen):
    files = sorted(glob.glob(gen + "/recomp_0*.c"))
    texts = {f: open(f, encoding="utf-8", errors="surrogateescape").read() for f in files}
    failed = 0
    for func, old, new in FIXES:
        done = False
        for f, s in texts.items():
            m = re.search(r"^void %s(?:_gen)?\(void\)\n\{\n.*?^\}" % func, s, re.S | re.M)
            if not m:
                continue
            body = m.group(0)
            if new in body:
                print("%s: already fixed" % func)
            elif body.count(old) == 1:
                texts[f] = s[:m.start()] + body.replace(old, new) + s[m.end():]
                print("%s: fixed (%s)" % (func, f.rsplit("/", 1)[-1]))
            else:
                print("%s: the lifted line is not there (%d found): fix it by hand" % (func, body.count(old)))
                failed = 1
            done = True
            break
        addr = int(func[4:], 16)
        if not done and "_flags /*" in old and any(
                int(a, 16) < addr < int(b, 16)
                for s in texts.values() for a, b in re.findall(r"^ \* Original: 0x(\w+) - 0x(\w+)", s, re.M)):
            # a caller-flags entry that is not a function of its own in this XBE (PAL's copy of the [esi+0xB0]
            # null check): it is lifted inside its caller, whose compare sets the flags, so there is nothing to fix
            print("%s: inside its caller in this XBE: the compare reaches it, nothing to fix" % func)
            done = True
        if not done:
            print("%s: function not found" % func)
            failed = 1
    for f, s in texts.items():
        open(f, "w", encoding="utf-8", errors="surrogateescape").write(s)
    return failed


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "src/recomp/gen"))
