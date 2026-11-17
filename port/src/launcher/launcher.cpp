// hpcos_launcher.exe: picks 4:3 or 16:9 and the graphics settings, writes them to bin\hpcos.toml and starts
// bin\hpcos.exe with the environment run_win.sh gives it (it runs hpcos_ntsc.dll or hpcos_pal.dll by the disc). Behind the page the title as text over a dark backdrop (no
// game art: the exes carry the port's own icon only). Dear ImGui over Direct3D 11.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "../single_instance.h"
#include "gamefiles.h"
#include "font/title_font.inc"  // Magic School One (FontMesa), the title's font
#include "xiso.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ── settings: hpcos.toml, the keys the game reads (renderer/app/app.cpp) ─────────────────────────────────────────────

struct Settings {
    bool wide = false;            // aspect '16:9', otherwise '4:3'
    int display = 0;              // 'windowed', 'borderless', 'fullscreen'
    uint32_t width = 1920, height = 1080;
    uint32_t internal_height = 0;
    uint32_t msaa = 0, aniso = 0;
    bool smaa = false, vsync = true;
    bool fsr = false;             // fsr_frame_generation: AMD FSR 3.1 frame generation (gpu/fsr_fg.cpp)
    bool show = true;             // show_launcher: false starts the game without this page
    int fps = 0;                  // fps_limit: 0 the game's, 1 30 fps
    int colorblind = 0;           // 'off', 'protanopia', 'deuteranopia', 'tritanopia'
    uint32_t cb_strength = 100;
    bool kbm = true;              // keyboard_mouse
    uint32_t mouse_sens = 50;
    bool vibration = true, invert_x = false, invert_y = false;
    bool controller = true;  // the in-game F10 option of the same name
    int language = 0;        // 'english' (US), 'english_uk', 'french', 'german', 'spanish', 'italian': the language the game starts in
    int speakers = 0;        // speakers: 0 stereo (2), 1 5.1 surround (6), as F10 > Audio > Speakers
    int opt[64] = {};        // the rest of F10's settings, by kOpts (each its own toml key)
};

// xboxrecomp: the rest of the game's settings (F10's, renderer/app/app.cpp's keys and defaults), so this page has them
// all (2026-11-14): each a toml key with its control. kHeader: a heading over the next ones; kCheckNot: a checkbox that
// is on when the key is false (Explore the grounds = no path warps)
enum OptType { kHeader, kCheck, kCheckNot, kSlider, kChoice };
const char* const kPromptNames[] = {"Follow the last input", "Controller buttons", "Keyboard keys"};
const char* const kMoveNames[] = {"Slow walk", "Walk", "Run"};
struct Opt {
    const char* tab;
    const char* key;  // null for a heading
    const char* label;
    OptType type;
    int def, lo, hi;
    const char* fmt;               // a slider's
    const char* const* names;      // a choice's
    const char* note;              // under it, greyed
};
const Opt kOpts[] = {
    {"Game", nullptr, "Enhancements", kHeader, 0, 0, 0, nullptr, nullptr, nullptr},
    {"Game", "grounds_warps", "Explore Hogwarts Grounds (experimental)", kCheckNot, 0, 0, 0, nullptr, nullptr,
     "On: walk the whole grounds. Off: a path's end fades to its other end, as the game does."},
    {"Game", "broom_land_flat", "Land the broom on any flat surface (experimental)", kCheck, 0, 0, 0, nullptr, nullptr,
     nullptr},
    {"Game", "per_pixel_lighting", "Per-pixel lighting", kCheck, 1, 0, 0, nullptr, nullptr,
     "Smooth light round Lumos and lamps. Off: per vertex, as the console."},
    {"Game", "cutscene_bars_hidden", "Cutscenes without black bars (16:9)", kCheck, 0, 0, 0, nullptr, nullptr, nullptr},
    {"Game", "skip_intro", "Skip the Warner Bros. and EA logos", kCheck, 0, 0, 0, nullptr, nullptr, nullptr},
    {"Game", "fog_disabled", "Disable fog", kCheck, 0, 0, 0, nullptr, nullptr, nullptr},
    {"Game", "loading_screens_4_3", "Keep loading screens 4:3 (16:9)", kCheck, 0, 0, 0, nullptr, nullptr, nullptr},
#ifndef HPCOS_RELEASE  // not in releases yet (v0.4.0, the user's call): the game plays them as the console does
    {"Game", "wider_ledge_catch", "Wider ledge catch", kCheck, 1, 0, 0, nullptr, nullptr,
     "A jump catches a ledge a little lower, higher or further than the game's narrow window."},
    {"Game", "ledge_climb_speed", "Ledge and climb speed %", kSlider, 150, 100, 300, "%d %%", nullptr,
     "How quickly Harry pulls himself up onto a ledge he caught; 100 as the game (about 4 s)."},
#endif
    {"Display", "fps_counter", "FPS counter", kCheck, 0, 0, 0, nullptr, nullptr, nullptr},
    {"Graphics", "render_percent", "Render scale %", kSlider, 100, 25, 200, "%d %%", nullptr,
     "Render scale and its limit apply with Internal resolution set to follow the window."},
    {"Graphics", "render_height_limit", "Max render height", kSlider, 0, 0, 4320, "%d lines", nullptr, nullptr},
    {"Graphics", "brightness", "Brightness", kSlider, 50, 0, 100, "%d", nullptr, nullptr},
    {"Graphics", "contrast", "Contrast", kSlider, 50, 0, 100, "%d", nullptr, nullptr},
    {"Graphics", "draw_all_objects", "Draw everything in view (no doorway culling)", kCheck, 1, 0, 0, nullptr, nullptr,
     nullptr},
    {"Graphics", "async_shaders", "Build shaders in the background", kCheck, 1, 0, 0, nullptr, nullptr,
     "A draw whose shader is still building is left out for a moment instead of waited for."},
    {"Post-processing", "bloom", "Bloom", kCheck, 0, 0, 0, nullptr, nullptr, "A glow round bright lights, not the HUD."},
    {"Post-processing", "bloom_strength", "Bloom strength", kSlider, 40, 0, 100, "%d", nullptr, nullptr},
    {"Post-processing", "sharpen", "Sharpening", kSlider, 0, 0, 100, "%d", nullptr, nullptr},
    {"Post-processing", "saturation", "Saturation", kSlider, 100, 0, 200, "%d %%", nullptr, nullptr},
    {"Post-processing", "vibrance", "Vibrance", kSlider, 0, 0, 100, "%d", nullptr, nullptr},
    {"Post-processing", "warmth", "Warmth", kSlider, 50, 0, 100, "%d", nullptr, "50 as the game, lower cooler, higher warmer."},
    {"Post-processing", "vignette", "Vignette", kSlider, 0, 0, 100, "%d", nullptr, nullptr},
    {"Post-processing", "grain", "Film grain", kSlider, 0, 0, 100, "%d", nullptr, nullptr},
    {"Post-processing", "fov", "Field of view % (experimental)", kSlider, 100, 80, 130, "%d %%", nullptr,
     "100 as the game."},
    {"Controls", "button_prompts", "Button prompts", kChoice, 0, 0, 2, nullptr, kPromptNames, nullptr},
    {"Controls", "mouse_invert_y", "Invert mouse Y", kCheck, 0, 0, 0, nullptr, nullptr, nullptr},
    {"Controls", "move_mode", "Movement keys (mouse wheel)", kChoice, 2, 0, 2, nullptr, kMoveNames, nullptr},
    {"Controls", nullptr, "Controller", kHeader, 0, 0, 0, nullptr, nullptr, nullptr},
    {"Controls", "deadzone_left", "Left stick deadzone %", kSlider, 10, 0, 50, "%d", nullptr, nullptr},
    {"Controls", "sensitivity_left", "Left stick sensitivity %", kSlider, 100, 25, 200, "%d", nullptr, nullptr},
    {"Controls", "deadzone_right", "Right stick deadzone %", kSlider, 10, 0, 50, "%d", nullptr, nullptr},
    {"Controls", "sensitivity_right", "Right stick sensitivity %", kSlider, 100, 25, 200, "%d", nullptr, nullptr},
    {"Controls", "vibration_strength", "Vibration strength %", kSlider, 100, 0, 100, "%d", nullptr, nullptr},
};
constexpr int kOptCount = int(sizeof(kOpts) / sizeof(kOpts[0]));
static_assert(kOptCount <= 64, "Settings::opt");

const char* const kColorBlindKeys[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
const char* const kColorBlindNames[] = {"Off", "Protanopia (red-weak)", "Deuteranopia (green-weak)", "Tritanopia (blue-weak)"};

const char* const kLanguageKeys[] = {"english", "english_uk", "french", "german", "spanish", "italian"};
const char* const kLanguageNames[] = {"English (US)", "English (UK)", "Fran\xC3\xA7" "ais", "Deutsch", "Espa\xC3\xB1ol", "Italiano"};

const char* const kSpeakerNames[] = {"Stereo", "5.1 surround"};

const char* const kDisplayKeys[] = {"windowed", "borderless", "fullscreen"};
const char* const kDisplayNames[] = {"Windowed", "Borderless", "Fullscreen"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"Off", "2x", "4x", "8x"};
constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"Game's", "2x", "4x", "8x", "16x"};

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

// bin\ beside this launcher: the game (hpcos.exe, its DLLs and the FSR ones) and everything it keeps for itself,
// hpcos.toml, shadercache\, logs\ and the file check's game_check.txt. game\, saves\ and licenses\ stay up here
std::wstring BinDir() { return ExeDir() + L"bin\\"; }

// a folder from before bin\ (everything beside the launcher): its settings, shader cache and file check moved in once,
// so an update keeps them and does not read the whole disc again. Something already in bin\ is left as it is
void MoveIntoBin() {
    std::wstring top = ExeDir(), bin = BinDir();
    CreateDirectoryW(bin.c_str(), nullptr);
    for (const wchar_t* name : {L"hpcos.toml", L"shadercache", L"game_check.txt", L"debug.env"}) {
        std::wstring from = top + name, to = bin + name;
        if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES)
            MoveFileW(from.c_str(), to.c_str());
    }
    // a release unpacked over an old folder already brings bin\shadercache (its base_*.bin), so the old cache was not
    // moved above: what the player's PC prepared (shaders.bin, the pipeline lists, a driver cache folder) goes in
    // entry by entry; the release's own base files win over the old ones, and the emptied old folder goes
    std::wstring old_cache = top + L"shadercache\\", new_cache = bin + L"shadercache\\";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((old_cache + L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring from = old_cache + n, to = new_cache + n;
            if (GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES) MoveFileW(from.c_str(), to.c_str());
            else if (n.rfind(L"base_", 0) == 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW(from.c_str());  // the release's copy is newer
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        RemoveDirectoryW((top + L"shadercache").c_str());  // only once empty
    }
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1) - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

// hpcos.toml as its lines, so a save keeps the comments and every key this page does not touch
std::vector<std::string> g_lines;

std::string Value(const std::string& key) {
    for (const auto& l : g_lines) {
        std::string t = Trim(l);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos || Trim(t.substr(0, eq)) != key) continue;
        std::string v = Trim(t.substr(eq + 1));
        if (v.size() >= 2 && (v[0] == '\'' || v[0] == '"')) v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

void SetValue(const std::string& key, const std::string& v) {
    for (auto& l : g_lines) {
        std::string t = Trim(l);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq != std::string::npos && Trim(t.substr(0, eq)) == key) {
            l = key + " = " + v;
            return;
        }
    }
    g_lines.push_back(key + " = " + v);
}

std::string TomlPath() { return Narrow(BinDir()) + "hpcos.toml"; }

Settings Load() {
    Settings s;
    g_lines.clear();
    std::ifstream f(TomlPath());
    for (std::string l; std::getline(f, l);) g_lines.push_back(l);
    auto u = [](const std::string& v, uint32_t d) { return v.empty() ? d : uint32_t(std::strtoul(v.c_str(), nullptr, 10)); };
    s.wide = Value("aspect") == "16:9";
    std::string dm = Value("display_mode");
    for (int i = 0; i < 3; i++)
        if (dm == kDisplayKeys[i]) s.display = i;
    s.width = u(Value("width"), s.width);
    s.height = u(Value("height"), s.height);
    s.internal_height = u(Value("internal_height"), 0);
    s.msaa = u(Value("msaa"), 0);
    s.aniso = u(Value("anisotropic_filtering"), 0);
    s.smaa = Value("smaa") == "true";
    s.vsync = Value("vsync") != "false";
    s.fsr = Value("fsr_frame_generation") == "true";
    s.show = Value("show_launcher") != "false";
    s.fps = u(Value("fps_limit"), 0) == 30 ? 1 : 0;
    std::string cb = Value("colorblind");
    for (int i = 0; i < 4; i++)
        if (cb == kColorBlindKeys[i]) s.colorblind = i;
    s.cb_strength = std::min(u(Value("colorblind_strength"), 100), 100u);
    s.kbm = Value("keyboard_mouse") != "false";
    s.mouse_sens = std::clamp(u(Value("mouse_sensitivity"), 50), 1u, 100u);
    s.vibration = Value("vibration") != "false";
    s.controller = Value("controller") != "false";
    s.invert_x = Value("invert_camera_x") == "true";
    s.invert_y = Value("invert_camera_y") == "true";
    std::string lang = Value("language");
    for (int i = 0; i < 6; i++)
        if (lang == kLanguageKeys[i]) s.language = i;
    s.speakers = u(Value("speakers"), 2) == 6 ? 1 : 0;
    for (int i = 0; i < kOptCount; i++) {
        const Opt& o = kOpts[i];
        if (!o.key) continue;
        std::string v = Value(o.key);
        if (o.type == kCheck || o.type == kCheckNot) s.opt[i] = v.empty() ? o.def : v == "true";
        else s.opt[i] = v.empty() ? o.def : std::clamp(int(std::strtol(v.c_str(), nullptr, 10)), o.lo, o.hi);
    }
    return s;
}

void Save(const Settings& s) {
    for (int i = 0; i < kOptCount; i++) {
        const Opt& o = kOpts[i];
        if (!o.key) continue;
        if (o.type == kCheck || o.type == kCheckNot) SetValue(o.key, s.opt[i] ? "true" : "false");
        else SetValue(o.key, std::to_string(s.opt[i]));
    }
    auto q = [](const char* v) { return std::string("'") + v + "'"; };
    SetValue("display_mode", q(kDisplayKeys[s.display]));
    SetValue("width", std::to_string(s.width));
    SetValue("height", std::to_string(s.height));
    SetValue("aspect", q(s.wide ? "16:9" : "4:3"));
    SetValue("internal_height", std::to_string(s.internal_height));
    SetValue("msaa", std::to_string(s.msaa));
    SetValue("smaa", s.smaa ? "true" : "false");
    SetValue("anisotropic_filtering", std::to_string(s.aniso));
    SetValue("vsync", s.vsync ? "true" : "false");
    SetValue("fsr_frame_generation", s.fsr ? "true" : "false");
    SetValue("show_launcher", s.show ? "true" : "false");
    SetValue("start_menu", "false");  // this page instead of the in-game one
    SetValue("fps_limit", "0");  // the 30 fps cap was removed: the game's own rate only
    SetValue("colorblind", q(kColorBlindKeys[s.colorblind]));
    SetValue("colorblind_strength", std::to_string(s.cb_strength));
    SetValue("keyboard_mouse", s.kbm ? "true" : "false");
    SetValue("mouse_sensitivity", std::to_string(s.mouse_sens));
    SetValue("vibration", s.vibration ? "true" : "false");
    SetValue("controller", s.controller ? "true" : "false");
    SetValue("invert_camera_x", s.invert_x ? "true" : "false");
    SetValue("invert_camera_y", s.invert_y ? "true" : "false");
    SetValue("language", q(kLanguageKeys[s.language]));
    SetValue("speakers", s.speakers ? "6" : "2");
    std::ofstream f(TomlPath(), std::ios::trunc);
    for (const auto& l : g_lines) f << l << "\n";
}

// sizes of the chosen shape, biggest first: the monitor's own and the common ones that fit the desktop
bool Shaped(uint32_t w, uint32_t h, bool wide) {
    float r = float(w) / float(h), want = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    return r > want - 0.02f && r < want + 0.02f;
}

// the monitor's biggest size. EnumDisplaySettings lists the real modes; GetSystemMetrics is scaled by Windows' DPI
// setting for a program that is not DPI-aware (a 4K screen at 150% reads 2560 x 1440 there), so it only stands in
std::pair<uint32_t, uint32_t> ScreenMax() {
    static const std::pair<uint32_t, uint32_t> max = [] {
        uint32_t w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        auto take = [&] {
            if (uint64_t(dm.dmPelsWidth) * dm.dmPelsHeight > uint64_t(w) * h) {
                w = dm.dmPelsWidth;
                h = dm.dmPelsHeight;
            }
        };
        for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++) take();
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) take();
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    return max;
}

// internal resolution: the lines the 3D is drawn at, 0 follows the window. Steps of the Xbox's 480 up to the screen's
// height, and that height itself; a saved value off the list (set in the toml) stays on it
struct InternalChoice {
    uint32_t lines;
    std::string name;
};

std::vector<InternalChoice> InternalChoices(uint32_t screen_h, uint32_t cur) {
    static const uint32_t kSteps[] = {480, 720, 960, 1080, 1440, 1920, 2160, 2880, 3240, 3840, 4320};
    std::vector<uint32_t> v;
    for (uint32_t l : kSteps)
        if (l <= screen_h) v.push_back(l);
    if (screen_h >= 480) v.push_back(screen_h);
    if (cur) v.push_back(cur);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::vector<InternalChoice> out{{0, "Follow window"}};
    for (uint32_t l : v) {
        uint32_t x = (l * 100 + 240) / 480;  // the scale over the Xbox's lines, in hundredths
        std::string scale = std::to_string(x / 100);
        if (x % 100) scale += "." + std::to_string(x % 100 / 10) + (x % 10 ? std::to_string(x % 10) : "");
        std::string name = std::to_string(l) + " (" + (l == 480 ? "Xbox, " : "") + scale + "x";
        if (l == screen_h) name += ", screen";
        out.push_back({l, name + ")"});
    }
    return out;
}

// the biggest size of a shape inside w x h, even on both sides (a 3440 x 1440 screen holds 2560 x 1440 at 16:9)
std::pair<uint32_t, uint32_t> FitShape(uint32_t w, uint32_t h, bool wide) {
    uint32_t rw = wide ? 16 : 4, rh = wide ? 9 : 3, k = std::min(w / rw, h / rh) & ~1u;
    return {k * rw, k * rh};
}

// up to the screen's biggest mode: its own modes of the shape, the biggest of the shape it holds, the common ones it
// holds; the screen's own size on top when it has another shape (16:10, ultrawide), the picture keeping its shape in it
std::vector<std::pair<uint32_t, uint32_t>> Sizes(bool wide) {
    static const std::pair<uint32_t, uint32_t> kCommon[] = {
        {640, 480},   {800, 600},   {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440},
        {2048, 1536}, {2880, 2160}, {1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800},
        {3840, 2160}, {5120, 2880}, {7680, 4320}, {3840, 2880}, {5760, 4320}};
    auto [sw, sh] = ScreenMax();
    std::vector<std::pair<uint32_t, uint32_t>> m;
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); i++)
        if (dm.dmPelsWidth >= 640 && Shaped(dm.dmPelsWidth, dm.dmPelsHeight, wide))
            m.emplace_back(dm.dmPelsWidth, dm.dmPelsHeight);
    auto fit = FitShape(sw, sh, wide);
    if (fit.first >= 640) m.push_back(fit);
    for (auto [w, h] : kCommon)
        if (Shaped(w, h, wide) && w <= sw && h <= sh) m.emplace_back(w, h);
    std::sort(m.begin(), m.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
    m.erase(std::unique(m.begin(), m.end()), m.end());
    if (!Shaped(sw, sh, wide)) m.insert(m.begin(), {sw, sh});
    return m;
}

// the size kept when it has the shape or is the screen's own, otherwise the biggest that has
void FitSize(Settings& s) {
    if (s.width && s.height && (Shaped(s.width, s.height, s.wide) || std::pair{s.width, s.height} == ScreenMax())) return;
    auto m = Sizes(s.wide);
    if (m.empty()) return;
    s.width = m.front().first;
    s.height = m.front().second;
}

// logs\runs keeps the newest 10 runs (each a game log and a kernel log, ~10 MB a session): every run made one, kept,
// so a player's folder grew without end; the newest ones are the ones a problem report needs. Names sort by time
static void PruneLogs(const std::wstring& runs) {
    for (const wchar_t* kind : {L"hpcos_run_*.log", L"xbox_kernel_*.log"}) {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((runs + kind).c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do names.push_back(fd.cFileName);
        while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(names.begin(), names.end());
        // this run's pair is made after: keep 9 so 10 are left
        for (size_t k = 0; k + 9 < names.size(); k++) DeleteFileW((runs + names[k]).c_str());
    }
}


// ── D3D12 under Wine ────────────────────────────────────────────────────────────────────────────────────────────────
// Wine's own D3D12 (its vkd3d, built into Wine) costs the render thread about 10 us a draw: the Hogwarts grounds drawn
// whole ran at 48 fps on it (2026-10-28). Proton and its forks (GE-Proton...) bring vkd3d-proton, which does not.
// Nothing is copied or overridden here: under Wine the page only says so when the d3d12.dll Wine hands out is its own
// builtin one (a Wine builtin carries "Wine builtin DLL" in its header).
bool UnderWine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

bool WineBuiltin(const wchar_t* dll) {
    HMODULE m = LoadLibraryW(dll);
    if (!m) return false;
    char header[0x200];
    memcpy(header, reinterpret_cast<const char*>(m), sizeof(header));
    FreeLibrary(m);
    for (size_t i = 0; i + 16 <= sizeof(header); i++)
        if (!memcmp(header + i, "Wine builtin DLL", 16)) return true;
    return false;
}

// what the page says about it ("" when all is well)
std::string D3D12Note() {
    if (!UnderWine() || !WineBuiltin(L"d3d12.dll")) return "";
    return "This Wine uses its own D3D12, which is slow with many draws: open areas such as the Hogwarts grounds drop "
           "below 60 fps. Run the game through Proton (Steam's Proton, GE-Proton or another Proton build), which "
           "brings vkd3d-proton.";
}

std::string FilesLogLine();

// ── the edition in game\ ────────────────────────────────────────────────────────────────────────────────────────────
// Both editions run from this folder: game\default.xbe says which (gamefiles_DetectEdition), read at start, before
// every check (after an unpack too) and before the game starts: bin\hpcos.exe loads hpcos_ntsc.dll for NTSC-US,
// hpcos_pal.dll for PAL, by the same check; each edition has its own list of the disc's files. Before there is a default.xbe (an image still to unpack) it reads as NTSC-US.
// The two discs carry the same six languages, so the Language list is the same for both.
int g_edition = GAME_EDITION_NONE;

void DetectEdition() {
    g_edition = gamefiles_DetectEdition((ExeDir() + L"game").c_str());
    gamefiles_SetEdition(g_edition);
}
bool Pal() { return g_edition == GAME_EDITION_PAL; }
// hpcos.exe for both editions (src/stub/stub.c); another edition goes to the NTSC-US DLL, whose edition check names
// what it found and what it needs
const wchar_t* GameExe() { return L"hpcos.exe"; }
const char* EditionLabel() {
    switch (g_edition) {
        case GAME_EDITION_NTSC: return "NTSC-US";
        case GAME_EDITION_PAL: return "PAL (Europe)";
        case GAME_EDITION_OTHER: return "not a supported edition";
        default: return "no default.xbe";
    }
}

// bin\\hpcos.exe with run_win.sh's environment, in this folder: the game finds game\\ and saves\\ from its working folder
bool StartGame() {
    // one started from elsewhere while this page was open: the log below would be truncated under it
    if (hpcos_GameRunning()) {
        hpcos_ReportAlreadyRunning();
        return true;
    }
    const std::pair<const wchar_t*, const wchar_t*> kEnv[] = {
        {L"RECOMP_TITLE_KEVENTS", L"1"}, {L"RECOMP_VBLANK", L"1"},       {L"RECOMP_AC97_READY", L"1"},
        {L"RECOMP_DSP_ACK", L"0x834D0810"}, {L"RECOMP_PB_EXEC", L"1"}, {L"RECOMP_FB_WINDOW", L"1"},
        {L"RECOMP_USB", L"1"},           {L"RECOMP_START_MENU", L"0"},  {L"RECOMP_ACK_SLEEP_US", L"250"},
    };
    for (auto [k, v] : kEnv)
        if (!GetEnvironmentVariableW(k, nullptr, 0)) SetEnvironmentVariableW(k, v);
    // the game refuses to start without it (single_instance.h)
    SetEnvironmentVariableW(HPCOS_LAUNCHER_ENV, L"1");
    DetectEdition();
    std::wstring dir = ExeDir(), exe = BinDir() + GameExe();
    std::wstring cmd = L"\"" + exe + L"\"";
    // the game's console output (its [CRASH] reports among it) to logs\runs\hpcos_run_<date_time>.log: a windowed
    // launcher has no console to hand down. One file per run, kept; the kernel's log beside it under the same time
    // (XBOX_KERNEL_LOG), and the name for F10's Debug tab (HPCOS_RUN_LOG)
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond);
    // in bin\\logs\\runs, named from this folder (the game's working folder)
    CreateDirectoryW(BinDir().c_str(), nullptr);
    CreateDirectoryW((BinDir() + L"logs").c_str(), nullptr);
    CreateDirectoryW((BinDir() + L"logs\\runs").c_str(), nullptr);
    PruneLogs(BinDir() + L"logs\\runs\\");
    std::wstring run_log = L"bin\\logs\\runs\\hpcos_run_" + std::wstring(stamp) + L".log";
    std::wstring kernel_log = L"bin\\logs\\runs\\xbox_kernel_" + std::wstring(stamp) + L".log";
    SetEnvironmentVariableW(L"HPCOS_RUN_LOG", run_log.c_str());
    SetEnvironmentVariableW(L"XBOX_KERNEL_LOG", kernel_log.c_str());
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW((dir + run_log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (log != INVALID_HANDLE_VALUE) {
        // the game folder's state first, so a log sent in says whether the files were whole
        std::string files = FilesLogLine();
        DWORD put = 0;
        WriteFile(log, files.data(), DWORD(files.size()), &put, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = si.hStdError = log;
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, dir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ── Disc image ──────────────────────────────────────────────────────────────────────────────────────────────────────
// game\ holds a disc image rather than its files: unpacked into game\ on a thread of its own before the game can
// start. Stopped or failed, the next start carries on where it stopped (files already whole are kept).

struct Unpack {
    XisoProgress p{};
    HANDLE thread = nullptr;
    bool ok = false;
    std::wstring image, dir;
    std::string name;  // the image's file name, for the page
};
Unpack g_unpack;

DWORD WINAPI UnpackThread(void*) {
    g_unpack.ok = xiso_Extract(g_unpack.image.c_str(), g_unpack.dir.c_str(), &g_unpack.p);
    return 0;
}

bool StartUnpack() {
    WCHAR image[MAX_PATH];
    g_unpack.dir = ExeDir() + L"game";
    if (!xiso_Find(g_unpack.dir.c_str(), image)) return false;
    g_unpack.image = image;
    g_unpack.name = Narrow(g_unpack.image.substr(g_unpack.image.find_last_of(L"\\/") + 1));
    g_unpack.thread = CreateThread(nullptr, 0, UnpackThread, nullptr, 0, nullptr);
    return g_unpack.thread != nullptr;
}

void DrawUnpack() {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##unpack", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Unpacking the game files");
    ImGui::TextWrapped("From %s into the game folder. This happens once; the disc image is left as it is and can "
                       "be deleted afterwards.", g_unpack.name.c_str());
    ImGui::Spacing();
    double done = double(g_unpack.p.done), total = double(g_unpack.p.total);
    char text[64];
    std::snprintf(text, sizeof text, "%.2f of %.2f GB", done / 1e9, total / 1e9);
    ImGui::ProgressBar(total > 0 ? float(done / total) : 0.0f, ImVec2(-FLT_MIN, 28 * sc), text);
    ImGui::Spacing();
    bool cancelling = g_unpack.p.cancel != 0;
    if (cancelling) ImGui::BeginDisabled();
    if (ImGui::Button("Cancel", ImVec2(160 * sc, 36 * sc))) InterlockedExchange(&g_unpack.p.cancel, 1);
    if (cancelling) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("The next start carries on where it stopped.");
    ImGui::End();
}

// ── Game files ──────────────────────────────────────────────────────────────────────────────────────────────────────
// game\ checked against the disc's files (gamefiles.h). Every byte is read once, on a fresh start (no game_check.txt
// made under this list) or after unpacking, and again only from the page's Verify and Repair buttons; every other
// start looks at the sizes alone (a few ms) and never reads them all on its own. The full check's result, the damaged
// files by name, is kept in game_check.txt beside the launcher and reported on later starts until a Verify or Repair
// clears it; a damaged or missing file is named on the page and in the run log (2026-10-29: a player's Burrow had no
// music, and nothing said whether their files were whole).

struct Check {
    GameCheck c{};
    HANDLE thread = nullptr;
    BOOL contents = FALSE;
    BOOL finished = FALSE;  // ran to the end (not cancelled, nothing failed)
    bool ran = false;       // a check has finished this session
    std::string date;       // when the contents were last checked, "" never
};
Check g_check;

std::wstring GameDir() { return ExeDir() + L"game"; }
std::wstring StampPath() { return BinDir() + L"game_check.txt"; }

std::string Today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char d[16];
    std::snprintf(d, sizeof d, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return d;
}

// the last full check, when it was made under this list
struct Stamp {
    bool valid = false;
    std::string date;
    std::vector<std::string> damaged;  // its files with wrong contents (missing and wrong-size ones a size check finds)
};

Stamp ReadStamp() {
    Stamp st;
    std::ifstream f(Narrow(StampPath()));
    char want[32];
    std::snprintf(want, sizeof want, "list %08lX", (unsigned long)gamefiles_ListId());
    std::string l;
    if (!std::getline(f, l) || Trim(l) != want) return st;
    while (std::getline(f, l)) {
        l = Trim(l);
        if (l.rfind("date ", 0) == 0) st.date = l.substr(5);
        else if (l.rfind("damaged ", 0) == 0) st.damaged.push_back(l.substr(8));
    }
    st.valid = !st.date.empty();
    return st;
}

void WriteStamp() {
    std::ofstream f(Narrow(StampPath()), std::ios::trunc);
    char list[32];
    std::snprintf(list, sizeof list, "list %08lX", (unsigned long)gamefiles_ListId());
    f << list << "\n"
      << "date " << g_check.date << "\n";
    for (int k = 0; k < g_check.c.listed; k++)
        if (g_check.c.kind[k] == GAMEFILE_CONTENT) f << "damaged " << gamefiles_Name(g_check.c.index[k]) << "\n";
    f << "# hpcos_launcher.exe's last full read of the game folder. Later starts check the sizes only; the launcher's\n"
         "# Verify game files button reads everything again (or delete this file to do it at the next start).\n";
}

DWORD WINAPI CheckThread(void*) {
    g_check.finished = gamefiles_Check(GameDir().c_str(), g_check.contents, &g_check.c);
    return 0;
}

bool StartCheck(bool contents) {
    DetectEdition();  // a fresh unpack or a swapped folder: the list of the edition now there
    g_check.c = GameCheck{};
    g_check.contents = contents;
    g_check.finished = FALSE;
    g_check.thread = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    return g_check.thread != nullptr;
}

bool FilesWhole() {
    return g_check.c.missing + g_check.c.wrong_size + g_check.c.wrong_content == 0;
}

// xboxrecomp: the player knows about these files (2026-11-14): the problems found, as the check names them, kept in the
// toml (files_problems_hidden) when they tick "I know about these files"; while the folder has exactly those, the report
// stays folded and the page does not open for them. Anything new (another file, another fault) brings it back
std::string ProblemSig() {
    char buf[64];
    snprintf(buf, sizeof buf, "%08lX:%d:%d:%d", (unsigned long)gamefiles_ListId(), g_check.c.missing,
             g_check.c.wrong_size, g_check.c.wrong_content);
    std::string sig = buf;
    for (int k = 0; k < g_check.c.listed; k++)
        sig += "," + std::to_string(g_check.c.index[k]) + "/" + std::to_string(g_check.c.kind[k]);
    return sig;
}

bool ProblemsHidden() {
    if (FilesWhole() || !g_check.finished) return false;
    const std::string v = Value("files_problems_hidden");
    return !v.empty() && v == ProblemSig();
}

// at once, not only with Play: a player who ticks it and quits finds it so next time
void HideProblems(bool hide) {
    SetValue("files_problems_hidden", hide ? "'" + ProblemSig() + "'" : std::string("''"));
    std::ofstream f(TomlPath(), std::ios::trunc);
    for (const auto& l : g_lines) f << l << "\n";
}

bool FilesOk() { return FilesWhole() || ProblemsHidden(); }

// after a check: a full one recorded; a size check gets the damage the last full one found added, the stamp kept
void FinishCheck(const Stamp* st = nullptr) {
    g_check.ran = true;
    if (!g_check.finished) return;
    if (g_check.contents) {
        g_check.date = Today();
        WriteStamp();
    } else if (st) {
        for (const std::string& name : st->damaged)
            gamefiles_AddResult(&g_check.c, gamefiles_Find(name.c_str()), GAMEFILE_CONTENT);
    }
}

const char* KindName(int kind) {
    return kind == GAMEFILE_MISSING ? "missing" : kind == GAMEFILE_SIZE ? "wrong size" : "damaged";
}

// the run log's first line
std::string FilesLogLine() {
    char head[200];
    std::string ed = std::string("[FILES] edition: ") + EditionLabel() + "\r\n";
    if (!g_check.ran) return ed + "[FILES] game folder not checked\r\n";
    if (!g_check.finished) {
        return ed + "[FILES] game folder check stopped: " + Narrow(g_check.c.error) + "\r\n";
    }
    std::snprintf(head, sizeof head, "[FILES] game folder: %d files listed, contents checked %s%s: ", gamefiles_Count(),
                  g_check.date.empty() ? "never" : g_check.date.c_str(), g_check.contents ? "" : ", sizes now");
    std::string line = ed + head;
    if (FilesWhole()) return line + "all match the disc\r\n";
    std::snprintf(head, sizeof head, "%d missing, %d wrong size, %d damaged:", g_check.c.missing, g_check.c.wrong_size,
                  g_check.c.wrong_content);
    line += head;
    for (int k = 0; k < g_check.c.listed; k++)
        line += std::string(" ") + gamefiles_Name(g_check.c.index[k]) + " (" + KindName(g_check.c.kind[k]) + ")";
    return line + "\r\n";
}

void DrawCheck() {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##check", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Checking the game files");
    ImGui::TextWrapped("Every file in the game folder is compared with the game disc's, so a damaged or missing one "
                       "is found before you play. This happens once.");
    ImGui::Spacing();
    double done = double(g_check.c.done), total = double(g_check.c.total);
    char text[64];
    std::snprintf(text, sizeof text, "%.2f of %.2f GB", done / 1e9, total / 1e9);
    ImGui::ProgressBar(total > 0 ? float(done / total) : 0.0f, ImVec2(-FLT_MIN, 28 * sc), text);
    ImGui::TextDisabled("%s", gamefiles_Name(g_check.c.current));
    ImGui::Spacing();
    bool cancelling = g_check.c.cancel != 0;
    if (cancelling) ImGui::BeginDisabled();
    if (ImGui::Button("Skip", ImVec2(160 * sc, 36 * sc))) InterlockedExchange(&g_check.c.cancel, 1);
    if (cancelling) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("It runs again at the next start.");
    ImGui::End();
}

enum class FilesAction { None, Verify, Repair };

// the page's game files part: a line when all is well, the files named and what to do when not
FilesAction DrawFiles(float sc) {
    FilesAction act = FilesAction::None;
    if (g_check.ran && g_check.finished && !FilesWhole() && !ProblemsHidden()) {
        int bad = g_check.c.missing + g_check.c.wrong_size + g_check.c.wrong_content;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.4f, 1));
        if (g_check.c.missing == gamefiles_Count())
            ImGui::TextWrapped("No game files found. Put the game's files, or its disc image (.iso), in the 'game' "
                               "folder beside the launcher.");
        else
            ImGui::TextWrapped("%d game file%s %s damaged or missing. The game may crash, or lose sound or "
                               "pictures, where it uses them:", bad, bad == 1 ? "" : "s", bad == 1 ? "is" : "are");
        ImGui::PopStyleColor();
        if (g_check.c.missing != gamefiles_Count()) {
            int shown = std::min(g_check.c.listed, 6);
            for (int k = 0; k < shown; k++)
                ImGui::BulletText("%s (%s)", gamefiles_Name(g_check.c.index[k]), KindName(g_check.c.kind[k]));
            if (bad > shown) ImGui::TextDisabled("...and %d more (all of them in the run log)", bad - shown);
            WCHAR image[MAX_PATH];
            if (xiso_FindAny(GameDir().c_str(), image)) {
                std::wstring w(image);
                std::string name = Narrow(w.substr(w.find_last_of(L"\\/") + 1));
                if (ImGui::Button(("Repair from " + name).c_str(), ImVec2(-FLT_MIN, 34 * sc))) act = FilesAction::Repair;
            } else {
                ImGui::TextWrapped("Copy them again from your disc or game dump, or put the disc image (.iso) in the "
                                   "'game' folder and the launcher writes them again from it.");
            }
        }
    }
    if (g_check.ran && g_check.finished && !FilesWhole()) {
        bool hidden = ProblemsHidden();
        if (hidden) {
            int bad = g_check.c.missing + g_check.c.wrong_size + g_check.c.wrong_content;
            ImGui::TextDisabled("%d known file problem%s hidden (untick to see them).", bad, bad == 1 ? "" : "s");
        }
        if (ImGui::Checkbox("I know about these files: don't show this again", &hidden)) HideProblems(hidden);
    }
    if (ImGui::Button("Verify game files", ImVec2(200 * sc, 0))) act = FilesAction::Verify;
    ImGui::SameLine();
    if (!g_check.ran) ImGui::TextDisabled("Game files not checked.");
    else if (!g_check.finished) ImGui::TextDisabled("Skipped: runs again next start.");
    else if (FilesWhole())
        ImGui::TextDisabled("All match the disc (checked %s).", g_check.date.c_str());
    else if (g_check.contents) ImGui::TextDisabled("Read through just now.");
    else ImGui::TextDisabled("Sizes checked; contents %s.", g_check.date.c_str());
    return act;
}

// ── Direct3D 11 ─────────────────────────────────────────────────────────────────────────────────────────────────────

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void MakeRtv() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    // xboxrecomp: d3d11 and what it loads (dxgi) from System32 only, not the exe's folder: a dxgi.dll left beside the
    // exes (DXVK's, copied there by launchers before 2026-10-28, or put there by a player for the game) was taken by
    // Wine's own d3d11, which then called DXGID3D10CreateDevice, a function DXVK's dxgi lacks: the launcher crashed
    // before its window opened (2026-10-29). Not linked, so nothing loads it before this.
    HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
                              reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")))
                        : nullptr;
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                 &g_swap, &g_dev, &fl, &g_ctx)))
        return false;
    MakeRtv();
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_swap && wp != SIZE_MINIMIZED) {
                if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
                g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                MakeRtv();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── the page ────────────────────────────────────────────────────────────────────────────────────────────────────────

template <size_t N>
int IndexOf(const uint32_t (&values)[N], uint32_t v) {
    for (size_t i = 0; i < N; i++)
        if (values[i] == v) return int(i);
    return 0;
}

ImFont* g_title_font = nullptr;  // Magic School One, for the title (WinMain)

// xboxrecomp (from the hpps port): a dark backdrop with the title as plain text in Magic School One, so the page says
// which game it starts without carrying the game's artwork
void DrawBackground() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float ww = io.DisplaySize.x, wh = io.DisplaySize.y;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ww, wh), IM_COL32(40, 96, 62, 255), IM_COL32(40, 96, 62, 255),
                                IM_COL32(16, 44, 28, 255), IM_COL32(16, 44, 28, 255));
    ImFont* f = g_title_font ? g_title_font : ImGui::GetFont();
    // "Harry Potter" on its own line, bigger; the rest of the title under it on one line, made smaller to fit if need be.
    // Magic School One draws small for its size, so both are bigger than a book face would be
    struct Line {
        const char* text;
        float size;
    };
    float big = std::min(wh * 0.15f, ww * 0.13f), rest = big * 0.62f;
    float rest_w = f->CalcTextSizeA(rest, FLT_MAX, 0, "and the Chamber of Secrets").x;
    if (rest_w > ww * 0.92f) rest *= ww * 0.92f / rest_w;
    const Line lines[2] = {{"Harry Potter", big}, {"and the Chamber of Secrets", rest}};
    float y = wh * 0.05f;
    for (const Line& l : lines) {
        ImVec2 t = f->CalcTextSizeA(l.size, FLT_MAX, 0, l.text);
        ImVec2 p((ww - t.x) * 0.5f, y);
        dl->AddText(f, l.size, ImVec2(p.x + 2, p.y + 2), IM_COL32(0, 0, 0, 200), l.text);
        dl->AddText(f, l.size, p, IM_COL32(222, 186, 92, 255), l.text);
        y += l.size * 0.9f;
    }
}

// returns true when Play is pressed (settings saved)
bool DrawPage(Settings& s, std::string& error, FilesAction& files) {
    ImGuiIO& io = ImGui::GetIO();
    float sc = io.FontGlobalScale;
    float pw = 560 * sc;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##page", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    // xboxrecomp: the settings in tabs, as F10's (2026-11-14): Game first (its language, the enhancements, the game
    // files), every setting the game has; the tabs' contents in a fixed height so the page keeps its place
    float bw = (pw - ImGui::GetStyle().WindowPadding.x * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    // the tabs fill what is left between the title (DrawBackground's two lines) and the page's foot, whose height is
    // taken from the frame before (the Play row, notes and messages under the tabs)
    static float foot = 260.0f * sc;
    const float big = std::min(io.DisplaySize.y * 0.15f, io.DisplaySize.x * 0.13f);
    const float title_end = io.DisplaySize.y * 0.05f + big * 0.9f + big * 0.62f * 0.9f + 16.0f * sc;
    const float tab_h = std::max(160.0f * sc, io.DisplaySize.y * 0.97f - title_end - foot);
    // a tab's own rows from kOpts
    auto opts = [&](const char* tab) {
        for (int i = 0; i < kOptCount; i++) {
            const Opt& o = kOpts[i];
            if (std::strcmp(o.tab, tab)) continue;
            switch (o.type) {
                case kHeader:
                    ImGui::Spacing();
                    ImGui::SeparatorText(o.label);
                    break;
                case kCheck: {
                    bool v = s.opt[i] != 0;
                    if (ImGui::Checkbox(o.label, &v)) s.opt[i] = v;
                    break;
                }
                case kCheckNot: {
                    bool v = s.opt[i] == 0;
                    if (ImGui::Checkbox(o.label, &v)) s.opt[i] = !v;
                    break;
                }
                case kSlider:
                    ImGui::SliderInt(o.label, &s.opt[i], o.lo, o.hi,
                                     !s.opt[i] && !std::strcmp(o.key, "render_height_limit") ? "No limit" : o.fmt);
                    break;
                case kChoice:
                    ImGui::Combo(o.label, &s.opt[i], o.names, o.hi - o.lo + 1);
                    break;
            }
            if (o.note) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::TextWrapped("%s", o.note);
                ImGui::PopStyleColor();
            }
        }
    };
    ImGui::PushItemWidth(pw * 0.5f);
    if (ImGui::BeginTabBar("##tabs")) {
        auto tab = [&](const char* name) {
            if (!ImGui::BeginTabItem(name)) return false;
            ImGui::BeginChild("##tab", ImVec2(0, tab_h), false);
            return true;
        };
        auto end_tab = [] {
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        if (tab("Game")) {
            ImGui::TextDisabled("Game disc: %s", EditionLabel());
            ImGui::Combo("Language", &s.language, kLanguageNames, 6);
            opts("Game");
            ImGui::Spacing();
            ImGui::SeparatorText("Game files");
            files = DrawFiles(sc);
            end_tab();
        }
        if (tab("Display")) {
            static const char* const kAspectNames[] = {"4:3 (original)", "16:9 (widescreen)"};
            int aspect = s.wide ? 1 : 0;
            if (ImGui::Combo("Aspect ratio", &aspect, kAspectNames, 2) && (aspect == 1) != s.wide) {
                s.wide = aspect == 1;
                FitSize(s);
            }
            ImGui::Combo("Display mode", &s.display, kDisplayNames, 3);
            {
                auto sizes = Sizes(s.wide);
                std::string cur = std::to_string(s.width) + " x " + std::to_string(s.height);
                if (ImGui::BeginCombo(s.display == 0 ? "Window size" : "Resolution", cur.c_str())) {
                    for (auto [w, h] : sizes) {
                        std::string name = std::to_string(w) + " x " + std::to_string(h);
                        if (std::pair{w, h} == ScreenMax()) name += "  (screen)";
                        if (ImGui::Selectable(name.c_str(), w == s.width && h == s.height)) {
                            s.width = w;
                            s.height = h;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::Checkbox("Vsync", &s.vsync);
            ImGui::Checkbox("FSR 3.1 frame generation", &s.fsr);
            if (s.fsr) ImGui::TextDisabled("A generated frame between each two of the game's (about 120 shown from its 60).");
            opts("Display");
            end_tab();
        }
        if (tab("Graphics")) {
            auto choices = InternalChoices(ScreenMax().second, s.internal_height);
            const InternalChoice* cur = &choices.front();
            for (const auto& c : choices)
                if (c.lines == s.internal_height) cur = &c;
            if (ImGui::BeginCombo("Internal resolution", cur->name.c_str())) {
                for (const auto& c : choices)
                    if (ImGui::Selectable(c.name.c_str(), &c == cur)) s.internal_height = c.lines;
                ImGui::EndCombo();
            }
            int ms = IndexOf(kMsaaValues, s.msaa);
            if (ImGui::Combo("MSAA", &ms, kMsaaNames, int(std::size(kMsaaNames)))) s.msaa = kMsaaValues[ms];
            ImGui::Checkbox("SMAA", &s.smaa);
            int an = IndexOf(kAnisoValues, s.aniso);
            if (ImGui::Combo("Anisotropic filtering", &an, kAnisoNames, int(std::size(kAnisoNames)))) s.aniso = kAnisoValues[an];
            opts("Graphics");
            end_tab();
        }
        if (tab("Post-processing")) {
            opts("Post-processing");
            end_tab();
        }
        if (tab("Audio")) {
            ImGui::Combo("Speakers", &s.speakers, kSpeakerNames, 2);
            if (s.speakers) ImGui::TextDisabled("5.1 needs Windows' speaker setup set to 5.1 (or a 5.1 HDMI receiver).");
            end_tab();
        }
        if (tab("Controls")) {
            ImGui::Checkbox("Keyboard and mouse", &s.kbm);
            int sens = int(s.mouse_sens);
            if (ImGui::SliderInt("Mouse sensitivity", &sens, 1, 100)) s.mouse_sens = uint32_t(sens);
            ImGui::Checkbox("Use controller", &s.controller);
            ImGui::Checkbox("Invert camera X", &s.invert_x);
            ImGui::SameLine(0, 30 * sc);
            ImGui::Checkbox("Invert camera Y", &s.invert_y);
            ImGui::Checkbox("Vibration", &s.vibration);
            opts("Controls");
            end_tab();
        }
        if (tab("Accessibility")) {
            ImGui::Combo("Colour-blind filter", &s.colorblind, kColorBlindNames, 4);
            if (s.colorblind) {
                int st = int(s.cb_strength);
                if (ImGui::SliderInt("Filter strength %", &st, 0, 100)) s.cb_strength = uint32_t(st);
            }
            end_tab();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::Spacing();
    bool skip = !s.show;
    if (ImGui::Checkbox("Don't show this at the next startup", &skip)) s.show = !skip;
    if (skip) ImGui::TextDisabled("Play starts the game directly. Hold Shift while starting to bring this back.");
    ImGui::Spacing();
    bool play = false;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button("Play", ImVec2(bw, 40 * sc))) play = true;
    ImGui::PopStyleColor(2);
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(bw, 40 * sc))) PostQuitMessage(0);
    if (!error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error.c_str());
    static const std::string d3d12_note = D3D12Note();
    if (!d3d12_note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.8f, 0.3f, 1));
        ImGui::TextWrapped("%s", d3d12_note.c_str());
        ImGui::PopStyleColor();
    }
    {   // this frame's own layout (where the last item ends, less the tabs' area): the window's height is a frame
        // late (it sizes to its contents), and following it made the page bounce
        const float f = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y - tab_h;
        if (std::fabs(f - foot) > 1.0f) foot = f;
    }
    ImGui::End();

    if (play) Save(s);
    return play;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    // xboxrecomp: a game already running: say so and show it, no settings page (its changes would not apply)
    if (hpcos_GameRunning()) {
        hpcos_ReportAlreadyRunning();
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MoveIntoBin();
    // xboxrecomp: a disc image to unpack first shows its progress page even with the settings page turned off
    bool unpacking = StartUnpack();
    // xboxrecomp: then the game files: read through when never checked whole under this list (a page with its
    // progress), otherwise their sizes only, here (a few ms)
    bool checking = false;
    if (!unpacking) {
        DetectEdition();  // the list the stamp and the check below are made under
        Stamp st = ReadStamp();
        if (!st.valid) {
            checking = StartCheck(true);  // a fresh start: every byte, once
        } else {
            g_check.date = st.date;
            g_check.contents = FALSE;
            g_check.finished = gamefiles_Check(GameDir().c_str(), FALSE, &g_check.c);
            FinishCheck(&st);
        }
    }
    // the page was turned off: straight to the game with the saved settings, unless Shift is held or a file is wrong
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!unpacking && !checking && !Load().show && FilesOk() && !shift && StartGame()) {
        CoUninitialize();
        return 0;
    }
    SetProcessDPIAware();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
                   nullptr, nullptr, L"hpcos_launcher", nullptr};
    RegisterClassExW(&wc);
    // 4:3, a good part of the screen
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ch = std::clamp(sh * 2 / 3, 600, 1200), cw = ch * 4 / 3;
    RECT rc{0, 0, cw, ch};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Harry Potter and the Chamber of Secrets", style, CW_USEDEFAULT,
                              CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!InitD3D(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 could not start.", L"Launcher", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.FontGlobalScale = std::max(1.0f, float(ch) / 600.0f);
    // xboxrecomp: the page's own font first (the default), then the title's, Magic School One, built into the exe
    // (font/title_font.inc); the default font stands in should it fail
    io.Fonts->AddFontDefault();
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;  // the exe's own bytes, not the atlas's to free
        g_title_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kTitleFont), int(sizeof kTitleFont),
                                                      64.0f, &cfg);
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6;
    ImGui::GetStyle().FrameRounding = 4;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    Settings s = Load();
    FitSize(s);
    std::string error;
    bool started = false;
    for (MSG msg{}; !started;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) goto done;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawBackground();
        if (unpacking) {
            DrawUnpack();
            if (WaitForSingleObject(g_unpack.thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(g_unpack.thread);
                g_unpack.thread = nullptr;
                unpacking = false;
                if (!g_unpack.ok)
                    error = Narrow(g_unpack.p.error) + " The game can't start until it is unpacked; open the launcher "
                            "again to carry on.";
                else
                    checking = StartCheck(true);  // what was written, read back against the list
            }
        } else if (checking) {
            DrawCheck();
            if (WaitForSingleObject(g_check.thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(g_check.thread);
                g_check.thread = nullptr;
                checking = false;
                FinishCheck();
                // the page was turned off: on to the game as before, when every file is whole
                if (g_check.finished && FilesOk() && !s.show && !shift && StartGame()) started = true;
            }
        } else if (FilesAction files = FilesAction::None; DrawPage(s, error, files)) {
            WCHAR image[MAX_PATH];
            if (xiso_Find((ExeDir() + L"game").c_str(), image))  // stopped part-way: not playable yet
                error = "The game files are not unpacked yet. Open the launcher again to carry on.";
            else if (StartGame()) started = true;
            else error = Narrow(GameExe()) + " could not be started (it must be in the bin folder beside the launcher).";
        } else if (files == FilesAction::Verify) {
            error.clear();
            checking = StartCheck(true);
        } else if (files == FilesAction::Repair) {
            // the damaged ones removed, then the image unpacked over the folder: only what is not whole is written
            WCHAR image[MAX_PATH];
            error.clear();
            if (xiso_FindAny(GameDir().c_str(), image)) {
                gamefiles_RemoveDamaged(GameDir().c_str(), &g_check.c);
                g_unpack = Unpack{};
                g_unpack.dir = GameDir();
                g_unpack.image = image;
                g_unpack.name = Narrow(g_unpack.image.substr(g_unpack.image.find_last_of(L"\\/") + 1));
                g_unpack.thread = CreateThread(nullptr, 0, UnpackThread, nullptr, 0, nullptr);
                unpacking = g_unpack.thread != nullptr;
            }
        }
        ImGui::Render();
        const float black[4] = {0, 0, 0, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, black);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
done:
    // closed while unpacking: stop it (the file being written is removed; the next start carries on)
    if (g_unpack.thread) {
        InterlockedExchange(&g_unpack.p.cancel, 1);
        WaitForSingleObject(g_unpack.thread, INFINITE);
        CloseHandle(g_unpack.thread);
    }
    // closed while checking: stopped, no stamp written, so the next start checks again
    if (g_check.thread) {
        InterlockedExchange(&g_check.c.cancel, 1);
        WaitForSingleObject(g_check.thread, INFINITE);
        CloseHandle(g_check.thread);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}
