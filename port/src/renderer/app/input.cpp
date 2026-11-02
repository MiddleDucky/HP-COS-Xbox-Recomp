// xboxrecomp: the controls settings (app.h) applied to every pad read, and keyboard and mouse as controller 1.
//
// hle_input.c's XInputGetState calls app_InputPad on the pad state the PC's controller gave: the game's buttons are
// remapped from the pad's (pad_map), each stick gets its deadzone and response, the camera stick its inversion, and
// on port 0 the keyboard and mouse are merged on top when keyboard_mouse is on: bound keys press their pad input,
// WASD tilt the left stick fully, the mouse's movement tilts the right stick by its speed. app_InputRumble scales the
// game's rumble. The keys and the mouse come from the framebuffer window's messages (menu.cpp's hook, InputMessage):
// a key table, and raw mouse input for movement, which keeps counting at the screen's edge. While the game window has
// the focus and no panel is open the pointer is held inside it, so a click cannot land on another window.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/app.h"
#include "input/xinput_xbox.h"

namespace app {

namespace {

struct KeyName_ {
    uint32_t vk;
    const char* name;
};
const KeyName_ kKeyNames[] = {
    {0, "none"},          {0x01, "Mouse1"},      {0x02, "Mouse2"},     {0x04, "Mouse3"},     {0x05, "Mouse4"},
    {0x06, "Mouse5"},     {0x08, "Backspace"},   {0x09, "Tab"},        {0x0D, "Enter"},      {0x10, "Shift"},
    {0x11, "Ctrl"},       {0x12, "Alt"},         {0x13, "Pause"},      {0x14, "CapsLock"},   {0x1B, "Escape"},
    {0x20, "Space"},      {0x21, "PageUp"},      {0x22, "PageDown"},   {0x23, "End"},        {0x24, "Home"},
    {0x25, "Left"},       {0x26, "Up"},          {0x27, "Right"},      {0x28, "Down"},       {0x2D, "Insert"},
    {0x2E, "Delete"},     {0x60, "Num0"},        {0x61, "Num1"},       {0x62, "Num2"},       {0x63, "Num3"},
    {0x64, "Num4"},       {0x65, "Num5"},        {0x66, "Num6"},       {0x67, "Num7"},       {0x68, "Num8"},
    {0x69, "Num9"},       {0x6A, "NumMultiply"}, {0x6B, "NumPlus"},    {0x6D, "NumMinus"},   {0x6E, "NumDecimal"},
    {0x6F, "NumDivide"},  {0xBA, "Semicolon"},   {0xBB, "Equals"},     {0xBC, "Comma"},      {0xBD, "Minus"},
    {0xBE, "Period"},     {0xBF, "Slash"},       {0xC0, "Backquote"},  {0xDB, "LeftBracket"}, {0xDC, "Backslash"},
    {0xDD, "RightBracket"}, {0xDE, "Quote"},
};

std::atomic<uint8_t> g_down[256];          // keys and mouse buttons held, by VK
std::atomic<long> g_mouse_dx{0}, g_mouse_dy{0};  // raw movement not yet read
std::atomic<bool> g_focus{false};
std::atomic<int> g_capture{-1};            // the menu waits for a key for this binding, -1 none
std::atomic<uint32_t> g_captured{~0u};     // the key it got
int g_wheel = 0;                           // wheel movement short of a notch
bool g_raw = false;                        // raw mouse input registered
bool g_clipped = false;

bool Held(uint32_t vk) { return vk && vk < 256 && g_down[vk].load(std::memory_order_relaxed); }
std::atomic<bool> g_last_kbm{true};  // xboxrecomp: keyboard and mouse used last (app_KeyboardPrompts), until a pad says otherwise

// the mouse button a message names, as VK
uint32_t MouseVk(unsigned msg, uintptr_t wp) {
    switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: return VK_LBUTTON;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: return VK_RBUTTON;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: return VK_MBUTTON;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: return HIWORD(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        default: return 0;
    }
}

int16_t Clamp16(float v) { return int16_t(std::lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

// a stick past its deadzone, rescaled to start from 0 there, times its response, direction kept
void Shape(int16_t* x, int16_t* y, uint32_t deadzone, uint32_t response) {
    float fx = *x / 32767.0f, fy = *y / 32767.0f, m = std::sqrt(fx * fx + fy * fy);
    float dz = std::min(deadzone, 90u) / 100.0f;
    if (m <= dz || m <= 0.0f) {
        *x = *y = 0;
        return;
    }
    float k = (m - dz) / (1.0f - dz) * (response / 100.0f) / m;
    float nx = fx * k, ny = fy * k, n = std::sqrt(nx * nx + ny * ny);
    if (n > 1.0f) nx /= n, ny /= n;
    *x = Clamp16(nx);
    *y = Clamp16(ny);
}

// the pad's inputs as 0..255 in kPadInputNames' order
void Read(const XBOX_GAMEPAD& g, uint8_t v[kPadInputs]) {
    for (int i = 0; i < 8; i++) v[i] = g.bAnalogButtons[i];
    for (int i = 0; i < 8; i++) v[8 + i] = (g.wButtons >> i & 1) ? 255 : 0;
}

void Write(XBOX_GAMEPAD& g, const uint8_t v[kPadInputs]) {
    for (int i = 0; i < 8; i++) g.bAnalogButtons[i] = v[i];
    g.wButtons &= ~0xFFu;
    for (int i = 0; i < 8; i++)
        if (v[8 + i] > XBOX_ANALOG_BUTTON_THRESHOLD) g.wButtons |= uint16_t(1u << i);
}

// the mouse's movement since the last read as a right stick tilt; read at most every 8 ms, so a title polling twice a
// frame sees the same tilt both times rather than the movement and then nothing
void MouseStick(const Settings& s, float* rx, float* ry) {
    static LARGE_INTEGER last{};
    static float sx, sy;
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    double dt = last.QuadPart ? double(now.QuadPart - last.QuadPart) / double(f.QuadPart) : 0.016;
    if (dt >= 0.008) {
        long dx = g_mouse_dx.exchange(0), dy = g_mouse_dy.exchange(0);
        // counts per second to a tilt: 2000 counts/s is full tilt at sensitivity 50
        float k = float(s.mouse_sensitivity) / 50.0f / 2000.0f;
        float nx = float(dx / std::max(dt, 0.004)) * k, ny = float(-dy / std::max(dt, 0.004)) * k;
        // a little smoothing: the poll and the mouse's reports do not line up
        sx = sx * 0.35f + nx * 0.65f;
        sy = sy * 0.35f + ny * 0.65f;
        if (std::fabs(sx) < 0.01f) sx = 0;
        if (std::fabs(sy) < 0.01f) sy = 0;
        last = now;
    }
    *rx = sx;
    *ry = s.mouse_invert_y ? -sy : sy;
}

}  // namespace

const char* KeyName(uint32_t vk) {
    for (const auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    static thread_local char buf[16];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = char(vk);
        buf[1] = 0;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(buf, sizeof buf, "F%u", vk - VK_F1 + 1);
    } else {
        std::snprintf(buf, sizeof buf, "0x%02X", vk);
    }
    return buf;
}

int KeyFromName(const char* name) {
    if (!name || !*name) return 0;
    for (const auto& k : kKeyNames)
        if (!_stricmp(name, k.name)) return int(k.vk);
    if (!name[1]) {
        char c = char(std::toupper(static_cast<unsigned char>(name[0])));
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return c;
    }
    if ((name[0] == 'F' || name[0] == 'f') && std::isdigit(static_cast<unsigned char>(name[1]))) {
        int n = std::atoi(name + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        long v = std::strtol(name, nullptr, 16);
        if (v > 0 && v < 256) return int(v);
    }
    return -1;
}

void InputMessage(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    if (!g_raw && hwnd) {
        RAWINPUTDEVICE rid{0x01, 0x02, 0, static_cast<HWND>(hwnd)};  // generic desktop, mouse
        g_raw = RegisterRawInputDevices(&rid, 1, sizeof rid) != FALSE;
    }
    switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (wp < 256) {
                if (g_capture >= 0 && wp != VK_F10) g_captured = uint32_t(wp);
                g_down[wp] = 1;
            }
            break;
        case WM_KEYUP: case WM_SYSKEYUP:
            if (wp < 256) g_down[wp] = 0;
            break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            if (g_capture >= 0) g_captured = MouseVk(msg, wp);
            g_down[MouseVk(msg, wp)] = 1;
            break;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            g_down[MouseVk(msg, wp)] = 0;
            break;
        case WM_MOUSEWHEEL: {
            // the movement keys' pace, a step a notch: up faster, down slower, held at slow walk and run. Not while the
            // panel is open (it scrolls) or keyboard and mouse are off
            Settings& s = Get();
            if (!s.kbm || MenuOpen()) break;
            g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
            int notches = g_wheel / WHEEL_DELTA;
            if (!notches) break;
            g_wheel -= notches * WHEEL_DELTA;
            uint32_t v = uint32_t(std::clamp(int(s.move_mode) + notches, 0, int(kMoveModes) - 1));
            if (v != s.move_mode) {
                s.move_mode = v;
                Save();
            }
            break;
        }
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof ri;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                    UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_mouse_dx += ri.data.mouse.lLastX;
                g_mouse_dy += ri.data.mouse.lLastY;
            }
            break;
        }
        case WM_SETFOCUS:
            g_focus = true;
            break;
        case WM_KILLFOCUS:
            g_focus = false;
            for (auto& d : g_down) d = 0;
            g_mouse_dx = g_mouse_dy = 0;
            break;
        case WM_ACTIVATE:
            g_focus = LOWORD(wp) != WA_INACTIVE;
            if (!g_focus)
                for (auto& d : g_down) d = 0;
            break;
        default:
            break;
    }
}

void InputFrame() {
    // the pointer held in the game window while keyboard and mouse play, the window has the focus and no panel is up
    HWND w = static_cast<HWND>(Window());
    bool want = Get().kbm && w && GetForegroundWindow() == w && !MenuOpen();
    if (want) {
        RECT r;
        if (GetClientRect(w, &r)) {
            POINT a{r.left, r.top}, b{r.right, r.bottom};
            ClientToScreen(w, &a);
            ClientToScreen(w, &b);
            RECT c{a.x, a.y, b.x, b.y};
            ClipCursor(&c);
            g_clipped = true;
        }
    } else if (g_clipped) {
        ClipCursor(nullptr);
        g_clipped = false;
    }
}

void StartKeyCapture(int binding) {
    g_captured = ~0u;
    g_capture = binding;
}

int KeyCaptureBinding() { return g_capture; }

bool TakeCapturedKey(uint32_t* vk) {
    uint32_t v = g_captured.exchange(~0u);
    if (v == ~0u) return false;
    g_capture = -1;
    *vk = v == VK_ESCAPE ? 0 : v;  // Escape clears the binding
    return true;
}

void CancelKeyCapture() { g_capture = -1; }

}  // namespace app

// hle_input.c: a pad read for the game, after the PC's controller filled it in. Returns which sticks keyboard and
// mouse drove (1 the left, 2 the right): those skip hle_input.c's stick range compensation, as they skip the deadzones
// and responses here, which are the controller's
extern "C" int app_InputPad(int port, XBOX_GAMEPAD* g) {
    int kbm_sticks = 0;
    const app::Settings& s = app::Get();
    if (!s.controller) std::memset(g, 0, sizeof(*g));  // xboxrecomp: "Use controller" off: the pad at rest
    uint8_t in[app::kPadInputs], out[app::kPadInputs];
    app::Read(*g, in);
    for (uint32_t i = 0; i < app::kPadInputs; i++) out[i] = in[std::min(s.pad_map[i], app::kPadInputs - 1)];
    app::Shape(&g->sThumbLX, &g->sThumbLY, s.deadzone_left, s.sensitivity_left);
    app::Shape(&g->sThumbRX, &g->sThumbRY, s.deadzone_right, s.sensitivity_right);
    if (port == 0 && s.kbm && GetForegroundWindow() == static_cast<HWND>(app::Window()) && !app::MenuOpen()) {
        for (uint32_t i = 0; i < app::kPadInputs; i++)
            if (app::Held(s.keys[i])) out[i] = 255;
        float lx = float(app::Held(s.keys[19])) - float(app::Held(s.keys[18]));
        float ly = float(app::Held(s.keys[16])) - float(app::Held(s.keys[17]));
        if (lx || ly) {
            // xboxrecomp: the keys' direction at the pace's speed (the mouse wheel's). The game takes each axis past
            // a deadzone of its own, v' = (|v| / 32768 - 0.35) / 0.65 (0x19050), and makes Harry's speed (record
            // +0x10C) 0.08 k, k = 0.8534 (e^m - 1) up to 1, m = |(x', y')| (0x62C0A). His gait follows the speed
            // through the table at 0x1E90C8: slow walk up to 0.04, walk up to 0.079, run from 0.05, and changes
            // only when the speed leaves the current gait's range (0x630D0). So the paces aim at 0.024 (slow walk),
            // 0.045 (walk: above slow walk's top, below run's bottom, so it is reached from both) and full speed.
            // m = ln(1 + k / 0.8534) gives k, and each axis is placed past its own deadzone: a diagonal moves as
            // fast as a straight line (it lost about a third to the two deadzones)
            constexpr float kK = 0.8534f, kDead = 0.35f;
            constexpr float kPaceSpeed[app::kMoveModes] = {0.30f, 0.5625f, 1.0f};
            float k = kPaceSpeed[std::min<uint32_t>(s.move_mode, app::kMoveModes - 1)];
            float m = std::log(1.0f + k / kK), len = std::sqrt(lx * lx + ly * ly);
            auto axis = [&](float d) -> int16_t {
                float v = m * d / len;
                if (v == 0.0f) return 0;
                float raw = std::min(kDead + (1.0f - kDead) * std::fabs(v), 1.0f) * 32768.0f;
                return int16_t(std::copysign(std::min(raw, 32767.0f), v));
            };
            g->sThumbLX = axis(lx);
            g->sThumbLY = axis(ly);
            kbm_sticks |= 1;
        }
        float rx, ry;
        app::MouseStick(s, &rx, &ry);
        rx += float(app::Held(s.keys[23])) - float(app::Held(s.keys[22]));
        ry += float(app::Held(s.keys[20])) - float(app::Held(s.keys[21]));
        if (rx || ry) {
            float m = std::sqrt(rx * rx + ry * ry);
            if (m > 1.0f) rx /= m, ry /= m;
            g->sThumbRX = app::Clamp16(rx);
            g->sThumbRY = app::Clamp16(ry);
            kbm_sticks |= 2;
        }
    }
    // xboxrecomp: which the player used last, for the button prompts (app_KeyboardPrompts): the controller's buttons,
    // triggers past a light touch or sticks past their deadzone (already shaped) against bound keys or the mouse
    if (port == 0) {
        bool pad = false, keys = false;
        for (uint32_t i = 0; i < app::kPadInputs; i++) pad |= in[i] > 40;
        pad |= g->sThumbLX || g->sThumbLY || g->sThumbRX || g->sThumbRY;
        if (s.kbm && GetForegroundWindow() == static_cast<HWND>(app::Window()) && !app::MenuOpen()) {
            for (uint32_t i = 0; i < app::kKeyBinds; i++) keys |= app::Held(s.keys[i]);
            keys |= (kbm_sticks & 2) != 0;
        }
        if (keys) app::g_last_kbm = true;
        else if (pad) app::g_last_kbm = false;
    }
    if (s.invert_x) g->sThumbRX = int16_t(std::max(-32767, -int(g->sThumbRX)));
    if (s.invert_y) g->sThumbRY = int16_t(std::max(-32767, -int(g->sThumbRY)));
    app::Write(*g, out);
    return kbm_sticks;
}

// xboxrecomp: the game's prompts show key names (hle_frontend.c's text, the HUD's button picture): button_prompts 2,
// or 0 with keyboard and mouse used last (and keyboard and mouse on)
extern "C" int app_KeyboardPrompts(void) {
    const app::Settings& s = app::Get();
    if (s.button_prompts == 2) return 1;
    if (s.button_prompts == 1 || !s.kbm) return 0;
    return app::g_last_kbm ? 1 : 0;
}

// xboxrecomp: the key bound to pad input i (A B X Y Black White LT RT Up Down Left Right Start Back LThumb RThumb),
// named for a prompt: "Space", "E", "Right Click"; empty when unbound
extern "C" const char* app_ButtonKeyLabel(int i) {
    const app::Settings& s = app::Get();
    if (i < 0 || uint32_t(i) >= app::kPadInputs) return "";
    uint32_t vk = s.keys[i];
    switch (vk) {
        case 0: return "";
        case 0x01: return "Left Click";
        case 0x02: return "Right Click";
        case 0x04: return "Middle Click";
        case 0x05: return "Mouse 4";
        case 0x06: return "Mouse 5";
        default: return app::KeyName(vk);
    }
}

// hle_input.c: the movement keys' pace (app::MoveMode)
extern "C" int app_MoveMode(void) { return int(std::min<uint32_t>(app::Get().move_mode, app::kMoveModes - 1)); }

// hle_input.c: keyboard and mouse play as controller 1 (port 0 is there with no controller plugged in)
extern "C" int app_KbmOn(void) { return app::Get().kbm; }

// hle_input.c: the game's rumble before it goes to the PC's controller
extern "C" void app_InputRumble(uint16_t* left, uint16_t* right) {
    const app::Settings& s = app::Get();
    uint32_t k = s.vibration && s.controller ? std::min(s.vibration_strength, 100u) : 0;
    *left = uint16_t(*left * k / 100);
    *right = uint16_t(*right * k / 100);
}
