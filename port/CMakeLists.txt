cmake_minimum_required(VERSION 3.20)

# ============================================================================
# hpcos - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(hpcos C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
# beside this folder (../xboxrecomp, a working copy) or around it (.., the GitHub repository: the toolkit at its
# top, the port in port/)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp/CMakeLists.txt")
    set(hpcos_toolkit_default "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp")
else()
    set(hpcos_toolkit_default "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
set(XBOXRECOMP_DIR "${hpcos_toolkit_default}" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# HPCOS_RELEASE: the build players get (tools/package_release.sh): stack canaries (-fstack-protector-strong) on the
# hand-written code (toolkit, HLE, renderer, launcher), SSE4.1 everywhere. The generated code is left without them
# below: its loads and stores go to guest memory through XBOX_PTR, which the checks cannot see, and it is the hot path.
# No _FORTIFY_SOURCE: HP:PS's release with it had a main-menu keyboard hitch on a player's PC that its builds without
# it did not (2026-10-25). ASLR, high-entropy VA and NX/DEP are on in every build (binutils defaults)
option(HPCOS_RELEASE "Release build for players: hardening" OFF)
if(HPCOS_RELEASE AND WIN32)
    add_compile_options(-msse4.1 -fstack-protector-strong)
    add_compile_definitions(HPCOS_RELEASE)
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # DirectSound on a host mixer (XAudio2) instead of the emulated APU.
    src/hle_dsound.c
    # The NV2A video overlay (XMV movies) shown by the host window.
    src/hle_video.c
    src/hle_crc.c
    # Rain drawn as one batch instead of a draw per drop (hand-written over the generated sub_000387F0).
    src/hle_rain.c
    # Trace wrappers around XAPI's controller calls (RECOMP_PAD_TRACE=1).
    src/hle_input.c
    src/hle_debug.c
    # The GameCube's GBA bonus: the Entrance Hall portrait into Gringotts (RECOMP_GRINGOTTS=0 = Xbox behaviour).
    src/hle_gringotts.c
    # Fading objects (Skurge sludge, cut curtains) drawn after the solid ones (RECOMP_FADE_LATE=0 = Xbox order).
    src/hle_fade.c
    # No chest sound for chests a level load puts back open (RECOMP_CHEST_LOAD_SOUND=1 = Xbox behaviour).
    src/hle_switch.c
    # The title's 2D sprites, placed for true 16:9.
    src/hle_hud.c
    # Cutscene shots that 16:9 shows more of than 4:3 should, zoomed to the 4:3 width (RECOMP_SHOT_ZOOM=0 = wide).
    src/hle_shots.c
    # The debug trainer's game side (hpcos_trainer.exe): noclip, health, spells, cards, beans, levels.
    src/hle_trainer.c
    # "Quit To Desktop" on the title menu (RECOMP_QUIT_ITEM=0 = Xbox menu).
    src/hle_frontend.c
    src/profiler.c
    # Direct3D 8 on a host D3D12 renderer, ported from the original renderer
    # (src/renderer, MIT): the XDK's D3D entry points instead of the NV2A.
    src/renderer/xdk/hle_d3d8.cpp
    # All four are hand-written over generated bodies: the recompile must pass
    #   --exclude-manual ../hpcos/src/hle_dsound.c
    #   --exclude-manual ../hpcos/src/hle_video.c
    #   --exclude-manual ../hpcos/src/hle_crc.c
    #   --exclude-manual ../hpcos/src/hle_input.c
    #   --exclude-manual ../hpcos/src/hle_debug.c
    #   --exclude-manual ../hpcos/src/hle_hud.c
    #   --exclude-manual ../hpcos/src/hle_shots.c
    #   --exclude-manual ../hpcos/src/hle_gringotts.c
    #   --exclude-manual ../hpcos/src/hle_fade.c
    #   --exclude-manual ../hpcos/src/hle_switch.c
    #   --exclude-manual ../hpcos/src/hle_trainer.c
    #   --exclude-manual ../hpcos/src/hle_frontend.c
    #   --exclude-manual ../hpcos/src/renderer/xdk/hle_d3d8.cpp
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

# resource 1: the port's own icon (res/app.ico, tools/make_icon.py: drawn from shapes, no game art), which
# fb_present.c and the launcher load for their windows; each exe also carries its version information (Explorer's
# Properties > Details)
# HPCOS_VERSION: the release's version (tools/package_release.sh passes VERSION), in Properties > Details as the file
# and product version; the description is the product's name, the copyright the port's author
set(HPCOS_VERSION "0.3.1" CACHE STRING "the release version, major.minor.patch")
string(REGEX MATCHALL "[0-9]+" hpcos_ver_parts "${HPCOS_VERSION}.0.0.0")
list(GET hpcos_ver_parts 0 hpcos_v0)
list(GET hpcos_ver_parts 1 hpcos_v1)
list(GET hpcos_ver_parts 2 hpcos_v2)
set(HPCOS_PRODUCT "Harry Potter and the Chamber of Secrets - unofficial PC port")
function(hpcos_version_rc out file)
    set(v "${hpcos_v0},${hpcos_v1},${hpcos_v2},0")
    set(${out} "1 VERSIONINFO\nFILEVERSION ${v}\nPRODUCTVERSION ${v}\nFILEOS 0x40004\nFILETYPE 1\nBEGIN\n  BLOCK \"StringFileInfo\"\n  BEGIN\n    BLOCK \"040904B0\"\n    BEGIN\n      VALUE \"CompanyName\", \"MiddleDucky\"\n      VALUE \"FileDescription\", \"${HPCOS_PRODUCT}\"\n      VALUE \"ProductName\", \"${HPCOS_PRODUCT}\"\n      VALUE \"FileVersion\", \"${HPCOS_VERSION}\"\n      VALUE \"ProductVersion\", \"${HPCOS_VERSION}\"\n      VALUE \"LegalCopyright\", \"Copyright (C) 2026 MiddleDucky\"\n      VALUE \"OriginalFilename\", \"${file}\"\n      VALUE \"InternalName\", \"${file}\"\n    END\n  END\n  BLOCK \"VarFileInfo\"\n  BEGIN\n    VALUE \"Translation\", 0x409, 1200\n  END\nEND\n" PARENT_SCOPE)
endfunction()
set(HPCOS_RC_ICON "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico\"\n")
hpcos_version_rc(HPCOS_RC_VERSION_GAME "hpcos.exe")
hpcos_version_rc(HPCOS_RC_VERSION_LAUNCHER "hpcos_launcher.exe")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hpcos_res.rc"
     CONTENT "// generated by CMakeLists.txt\n${HPCOS_RC_ICON}${HPCOS_RC_VERSION_GAME}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/launcher_res.rc"
     CONTENT "// generated by CMakeLists.txt\n${HPCOS_RC_ICON}${HPCOS_RC_VERSION_LAUNCHER}")
# the icon is named by these scripts, not part of them: without this a new res/app.ico left the old one in the exes
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/hpcos_res.rc" "${CMAKE_CURRENT_BINARY_DIR}/launcher_res.rc" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/hpcos_res.rc
)

# MinGW GCC 16 at -O3 sometimes hoists the address of a thread local array out of a loop as a plain global: the
# lifted code then reads the TLS template instead of the thread's copy (g_fp_stack in sub_00036EC0, which turned the
# Whomping Willow's right hand to angle 0). -O2 keeps it out; the post build check below fails on any such access.
if(WIN32)
    if(HPCOS_RELEASE)
        set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-fno-stack-protector")
    else()
        set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_tls_refs.py
                    --objdump ${CMAKE_OBJDUMP} --nm ${CMAKE_NM} $<TARGET_FILE:${PROJECT_NAME}>
            VERBATIM)
    endif()
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# The D3D12 renderer (src/renderer/gpu; MIT, src/renderer/LICENSE): XDK Direct3D state to D3D12.
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE
        src/renderer/app/app.cpp
        src/renderer/gpu/d3d12.cpp
        src/renderer/gpu/pipeline_cache.cpp
        src/renderer/gpu/render_thread.cpp
        src/renderer/gpu/shaders.cpp
        src/renderer/gpu/texture.cpp
        src/renderer/gpu/vertex.cpp
        src/renderer/app/menu.cpp
        src/renderer/app/input.cpp
    )
    # Dear ImGui v1.91.0 (third_party/imgui, MIT), as the renderer: the F10 options panel
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    target_sources(${PROJECT_NAME} PRIVATE
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
    set_source_files_properties(
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${IMGUI_DIR}" COMPILE_OPTIONS "-O2")
    set_source_files_properties(
        src/renderer/xdk/hle_d3d8.cpp src/renderer/app/app.cpp src/renderer/gpu/d3d12.cpp
        src/renderer/gpu/pipeline_cache.cpp src/renderer/gpu/render_thread.cpp src/renderer/gpu/shaders.cpp
        src/renderer/gpu/texture.cpp src/renderer/gpu/vertex.cpp src/renderer/app/menu.cpp src/renderer/app/input.cpp
        PROPERTIES INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer;${XBOXRECOMP_DIR}/src;${IMGUI_DIR}"
        COMPILE_OPTIONS "-msse4.1")
    set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()

if(WIN32)
    # the C++ renderer must not need MinGW's libstdc++/libgcc/winpthread DLLs at run time
    target_link_options(hpcos PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_libraries(hpcos PRIVATE d3d11 d3d12 d3dcompiler dxgi dxguid xinput1_4 winmm dbghelp xaudio2_8 ole32 imm32 dwmapi gdi32 shell32)
else()
    target_link_options(hpcos PRIVATE -rdynamic)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# HPCOS_CALL_PROFILE: count every direct call by target (F7 dumps calls_N.txt, see profiler.c)
option(HPCOS_CALL_PROFILE "Count direct calls per target, F7 dumps" OFF)
if(HPCOS_CALL_PROFILE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_CALL_PROFILE)
endif()

# HPCOS_DIFFTEST: compare every called function with its original x86 in Unicorn (F5 sweep, difftest.txt; see
# src/difftest.c). Needs unicorn.dll next to the executable.
option(HPCOS_DIFFTEST "Differential test against the original x86 (Unicorn)" OFF)
if(HPCOS_DIFFTEST)
    target_sources(${PROJECT_NAME} PRIVATE src/difftest.c)
    target_include_directories(${PROJECT_NAME} PRIVATE third_party/unicorn)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_DIFFTEST)
endif()

# hpcos_trainer.exe: the debug trainer beside the game (noclip, health, spells, cards, beans, levels, saved spots);
# src/trainer/trainer.c, the game side is src/hle_trainer.c
if(WIN32)
    add_executable(hpcos_trainer WIN32 src/trainer/trainer.c)
    target_compile_options(hpcos_trainer PRIVATE -O2)
    target_link_options(hpcos_trainer PRIVATE -static -static-libgcc -mwindows)
    target_link_libraries(hpcos_trainer PRIVATE comctl32 gdi32 user32)
endif()

# hpcos_launcher.exe: 4:3 or 16:9 and the graphics settings, then hpcos.exe (src/launcher/launcher.cpp)
if(WIN32)
    add_executable(hpcos_launcher WIN32
        src/launcher/launcher.cpp
        src/launcher/xiso.c  # unpacks a disc image in game\ before the first start
        src/launcher/gamefiles.c  # checks game\ against the disc's files (game_files.inc)
        ${CMAKE_CURRENT_BINARY_DIR}/launcher_res.rc
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(hpcos_launcher PRIVATE ${IMGUI_DIR})
    set_target_properties(hpcos_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_compile_options(hpcos_launcher PRIVATE -O2)
    target_link_options(hpcos_launcher PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
    target_link_libraries(hpcos_launcher PRIVATE d3d11 dxgi d3dcompiler ole32 imm32 dwmapi gdi32 xinput1_4)
endif()
