/* xboxrecomp (from the hpps port): one copy of the game at a time. hpcos.exe holds a named mutex for its whole life;
 * a second hpcos.exe, or the launcher while one runs, says so, brings the running game's window forward and starts
 * nothing. A second copy would share saves\, shadercache\ and hpcos_run.log with the first (and the launcher would
 * truncate its log). */
#ifndef HPCOS_SINGLE_INSTANCE_H
#define HPCOS_SINGLE_INSTANCE_H

#include <wchar.h>
#include <windows.h>

#define HPCOS_RUNNING_MUTEX L"Local\\hpcos_game_running"
#define HPCOS_GAME_WINDOW_CLASS "XboxRecompFramebuffer" /* fb_present.c */
#define HPCOS_TITLE L"Harry Potter and the Chamber of Secrets"

/* the message, then the running game's window to the front (after the box: a fullscreen game would cover it) */
static inline void hpcos_ReportAlreadyRunning(void)
{
    MessageBoxW(NULL, HPCOS_TITLE L" is already running.", HPCOS_TITLE,
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    HWND w = FindWindowA(HPCOS_GAME_WINDOW_CLASS, NULL);
    if (w) {
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
    }
}

/* the game: takes the mutex for the rest of the process; 0 when another copy already has it */
static inline int hpcos_ClaimSingleInstance(void)
{
    HANDLE m = CreateMutexW(NULL, FALSE, HPCOS_RUNNING_MUTEX);
    return !(m && GetLastError() == ERROR_ALREADY_EXISTS);
}

/* whether a game holds the mutex now */
static inline int hpcos_GameRunning(void)
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, HPCOS_RUNNING_MUTEX);
    if (!m) return 0;
    CloseHandle(m);
    return 1;
}

/* hpcos.exe runs only from hpcos_launcher.exe, which sets this in the game's environment (with its RECOMP_* settings,
 * which a bare start would lack); the run scripts set it themselves */
#define HPCOS_LAUNCHER_ENV L"HPCOS_LAUNCHER"

static inline int hpcos_StartedByLauncher(void)
{
    return GetEnvironmentVariableW(HPCOS_LAUNCHER_ENV, NULL, 0) != 0;
}

/* hpcos.exe started on its own: says to use the launcher and offers to open it (in the folder above bin\, where
 * this exe is; beside it in a folder from before bin\) */
static inline void hpcos_OfferLauncher(void)
{
    int r = MessageBoxW(NULL, L"Please start the game with hpcos_launcher.exe.\n\nOpen the launcher now?", HPCOS_TITLE,
                        MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    if (r != IDYES) return;
    wchar_t dir[MAX_PATH], exe[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : NULL;
    if (!slash) return;
    slash[1] = 0;
    lstrcpyW(exe, dir);
    lstrcatW(exe, L"hpcos_launcher.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES && slash > dir) {
        wchar_t *up = slash - 1;  /* the folder above */
        while (up > dir && *up != L'\\') up--;
        if (*up == L'\\') {
            up[1] = 0;
            lstrcpyW(exe, dir);
            lstrcatW(exe, L"hpcos_launcher.exe");
        }
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(NULL, L"hpcos_launcher.exe was not found.", HPCOS_TITLE,
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }
}

#endif
