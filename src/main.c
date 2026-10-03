/*
 * mhide - auto-hide the mouse cursor on Windows.
 *
 * Plain Win32, statically linked, no external dependencies.
 *
 * Hiding: every system cursor is replaced with a fully transparent one via
 * SetSystemCursor, and the user's cursor scheme is restored via SPI_SETCURSORS.
 *
 * Resource use is kept minimal by never polling while the cursor is visible:
 *   - visible: a single coalescable timer fires at the idle deadline. When it
 *     fires it re-checks GetLastInputInfo and reschedules if input occurred.
 *   - hidden:  Raw Input (RIDEV_INPUTSINK) for mouse + keyboard is registered
 *     so the first real input event wakes us; it is unregistered on show.
 *     A slow 250 ms fallback check of GetLastInputInfo catches anything else.
 *
 * Safety: because the blanked cursors are shared system state, a watchdog
 * helper process (this same exe, "--watchdog <pid>") restores the cursor
 * scheme if this process dies for any reason, and "--restore" does so manually.
 */

#define WIN32_LEAN_AND_MEAN
#define OEMRESOURCE
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <wchar.h>
#include <stdarg.h>
#include "resource.h"

#define APP_NAME            L"mhide"
#define WND_CLASS           L"mhideTrayWindow"
#define MUTEX_NAME          L"Local\\mhide-single-instance"
#define REG_SETTINGS_KEY    L"Software\\mhide"
#define REG_RUN_KEY         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define WM_TRAYICON         (WM_APP + 1)
#define WM_OPEN_SETTINGS    (WM_APP + 2)

#define TIMER_HIDE          1   /* fires when the cursor is due to hide */
#define TIMER_FALLBACK      2   /* slow input check while hidden */
#define TIMER_DLG_STATUS    1

#define FALLBACK_INTERVAL_MS 250   /* only runs while hidden, i.e. while the user is away */
#define TIMER_TOLERANCE_MS   250

#define MIN_DELAY_SECONDS   1
#define MAX_DELAY_SECONDS   86400

#define HID_USAGE_PAGE_GENERIC  0x01
#define HID_USAGE_MOUSE         0x02
#define HID_USAGE_KEYBOARD      0x06

typedef struct Settings {
    DWORD delaySeconds;
    DWORD enabled;       /* 0/1 */
} Settings;

static HINSTANCE        g_hInst;
static HWND             g_hWnd;
static HWND             g_hDlg;             /* modeless settings dialog, or NULL */
static HICON            g_hIcon;
static NOTIFYICONDATAW  g_nid;
static UINT             g_taskbarCreatedMsg;
static Settings         g_cfg = { 30, 1 };

static BOOL             g_hidden;           /* TRUE while system cursors are blanked */
static DWORD            g_hiddenInputTick;  /* GetLastInputInfo value when we hid */

static HANDLE           g_hWatchdog;        /* helper process that restores cursors if we die */
static HANDLE           g_hLog = INVALID_HANDLE_VALUE; /* set MHIDE_LOG=<path> to trace events */

static void ScheduleHide(void);

/* --------------------------------------------------------------- tracing */

static void OpenLog(void)
{
    WCHAR path[MAX_PATH];
    if (GetEnvironmentVariableW(L"MHIDE_LOG", path, ARRAYSIZE(path)) == 0) return;
    g_hLog = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void Log(const char *fmt, ...)
{
    char line[256];
    size_t len = 0;
    DWORD written;
    va_list args;

    if (g_hLog == INVALID_HANDLE_VALUE) return;
    StringCchPrintfA(line, ARRAYSIZE(line), "%10lu  ", GetTickCount());
    StringCchLengthA(line, ARRAYSIZE(line), &len);
    va_start(args, fmt);
    StringCchVPrintfA(line + len, ARRAYSIZE(line) - len, fmt, args);
    va_end(args);
    StringCchCatA(line, ARRAYSIZE(line), "\r\n");
    StringCchLengthA(line, ARRAYSIZE(line), &len);
    WriteFile(g_hLog, line, (DWORD)len, &written, NULL);
}

/* ---------------------------------------------------------------- watchdog */

/*
 * SetSystemCursor changes shared system state that outlives this process, so a
 * crash or a forced kill while hidden would leave the user without a cursor.
 * A second copy of this exe waits on our process handle and restores the
 * cursor scheme the moment we exit for any reason. It sleeps in a kernel wait
 * and uses no CPU.
 */
static int RunWatchdog(DWORD parentPid)
{
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (parent) {
        WaitForSingleObject(parent, INFINITE);
        CloseHandle(parent);
    }
    SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);
    return 0;
}

static void EnsureWatchdog(void)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (g_hWatchdog) {
        if (WaitForSingleObject(g_hWatchdog, 0) == WAIT_TIMEOUT)
            return; /* still alive */
        CloseHandle(g_hWatchdog);
        g_hWatchdog = NULL;
    }

    GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\" --watchdog %lu", exe, GetCurrentProcessId());
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        g_hWatchdog = pi.hProcess;
    }
}

/* ------------------------------------------------------------------ input */

static DWORD GetLastInputTick(void)
{
    LASTINPUTINFO lii;
    lii.cbSize = sizeof lii;
    lii.dwTime = 0;
    GetLastInputInfo(&lii);
    return lii.dwTime;
}

static void RegisterInputSink(BOOL enable)
{
    RAWINPUTDEVICE rid[2];
    DWORD flags = enable ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    HWND target = enable ? g_hWnd : NULL;

    rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid[0].usUsage = HID_USAGE_MOUSE;
    rid[0].dwFlags = flags;
    rid[0].hwndTarget = target;
    rid[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid[1].usUsage = HID_USAGE_KEYBOARD;
    rid[1].dwFlags = flags;
    rid[1].hwndTarget = target;
    if (!RegisterRawInputDevices(rid, ARRAYSIZE(rid), sizeof rid[0]))
        Log("RegisterRawInputDevices(%s) failed: %lu", enable ? "sink" : "remove", GetLastError());
    else
        Log("raw input %s", enable ? "registered" : "removed");
}

/*
 * Every WM_INPUT counts as activity. Injected mouse moves (SendInput, remote
 * desktop tools) can arrive with zero deltas, so no filtering on the payload.
 */
static void TraceInput(HRAWINPUT hRaw)
{
    RAWINPUT raw;
    UINT size = sizeof raw;
    if (g_hLog == INVALID_HANDLE_VALUE) return;
    if (GetRawInputData(hRaw, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        Log("WM_INPUT: GetRawInputData failed");
    } else if (raw.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &raw.data.mouse;
        Log("WM_INPUT mouse dx=%ld dy=%ld buttons=%u flags=%u", m->lLastX, m->lLastY,
            m->usButtonFlags, m->usFlags);
    } else {
        Log("WM_INPUT type=%lu", raw.header.dwType);
    }
}

/* ------------------------------------------------------------------ cursor */

static const DWORD kSystemCursorIds[] = {
    OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP,
    OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL,
    OCR_NO, OCR_HAND, OCR_APPSTARTING, 32651 /* OCR_HELP */,
    32671 /* OCR_PIN */, 32672 /* OCR_PERSON */
};

static HCURSOR CreateBlankCursor(void)
{
    /* 32x32 monochrome: AND mask all 1s + XOR mask all 0s = fully transparent. */
    BYTE andMask[32 * 4];
    BYTE xorMask[32 * 4];
    memset(andMask, 0xFF, sizeof andMask);
    memset(xorMask, 0x00, sizeof xorMask);
    return CreateCursor(g_hInst, 0, 0, 32, 32, andMask, xorMask);
}

static void HideSystemCursor(void)
{
    size_t i;
    if (g_hidden) return;

    EnsureWatchdog();
    KillTimer(g_hWnd, TIMER_HIDE);

    /*
     * Order matters: register for Raw Input first, then blank the cursors, then
     * snapshot the last-input tick. Any input from here on either arrives as
     * WM_INPUT or is newer than the snapshot, so nothing can slip through the
     * gap and leave the cursor hidden until the next input.
     */
    RegisterInputSink(TRUE);
    for (i = 0; i < ARRAYSIZE(kSystemCursorIds); i++) {
        HCURSOR blank = CreateBlankCursor();
        if (!blank) continue;
        /* SetSystemCursor takes ownership of the handle on success. */
        if (!SetSystemCursor(blank, kSystemCursorIds[i]))
            DestroyCursor(blank);
    }
    g_hidden = TRUE;
    g_hiddenInputTick = GetLastInputTick();
    Log("HIDE (last input tick %lu)", g_hiddenInputTick);
    SetCoalescableTimer(g_hWnd, TIMER_FALLBACK, FALLBACK_INTERVAL_MS, NULL, FALLBACK_INTERVAL_MS / 2);
}

static void ShowSystemCursor(void)
{
    if (!g_hidden) return;

    RegisterInputSink(FALSE);
    KillTimer(g_hWnd, TIMER_FALLBACK);
    SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);
    g_hidden = FALSE;
    Log("SHOW");
    ScheduleHide();
}

/* Called whenever user activity is detected while hidden. */
static void OnActivity(void)
{
    if (g_hidden) ShowSystemCursor();
}

/* ------------------------------------------------------------- scheduling */

/* Milliseconds until the cursor will hide, or 0 if hidden/disabled/due now. */
static DWORD MillisUntilHide(void)
{
    DWORD now, elapsed, delay;
    if (!g_cfg.enabled || g_hidden) return 0;
    now = GetTickCount();
    elapsed = now - GetLastInputTick();
    delay = g_cfg.delaySeconds * 1000;
    return (elapsed >= delay) ? 0 : (delay - elapsed);
}

/* (Re)arms the single hide timer, or hides immediately if already due. */
static void ScheduleHide(void)
{
    DWORD ms;
    KillTimer(g_hWnd, TIMER_HIDE);
    if (!g_cfg.enabled || g_hidden) return;
    ms = MillisUntilHide();
    Log("schedule: hide in %lu ms", ms);
    if (ms == 0) {
        HideSystemCursor();
        return;
    }
    SetCoalescableTimer(g_hWnd, TIMER_HIDE, max(ms, USER_TIMER_MINIMUM), NULL, TIMER_TOLERANCE_MS);
}

/* ---------------------------------------------------------------- settings */

static void LoadSettings(void)
{
    HKEY key;
    DWORD value, size;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    size = sizeof value;
    if (RegQueryValueExW(key, L"DelaySeconds", NULL, NULL, (BYTE *)&value, &size) == ERROR_SUCCESS)
        g_cfg.delaySeconds = min(max(value, MIN_DELAY_SECONDS), MAX_DELAY_SECONDS);
    size = sizeof value;
    if (RegQueryValueExW(key, L"Enabled", NULL, NULL, (BYTE *)&value, &size) == ERROR_SUCCESS)
        g_cfg.enabled = value ? 1 : 0;

    RegCloseKey(key);
}

static void SaveSettings(void)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, L"DelaySeconds", 0, REG_DWORD, (const BYTE *)&g_cfg.delaySeconds, sizeof g_cfg.delaySeconds);
    RegSetValueExW(key, L"Enabled", 0, REG_DWORD, (const BYTE *)&g_cfg.enabled, sizeof g_cfg.enabled);
    RegCloseKey(key);
}

static BOOL IsAutostartEnabled(void)
{
    HKEY key;
    BOOL result = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(key, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
        result = TRUE;
    RegCloseKey(key);
    return result;
}

static void SetAutostart(BOOL enable)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    if (enable) {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH + 2];
        GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
        StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\"", exe);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((lstrlenW(cmd) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}

static void UpdateTrayTooltip(void)
{
    if (g_cfg.enabled) {
        StringCchPrintfW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"mhide - hide after %u s of inactivity",
                         g_cfg.delaySeconds);
    } else {
        StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"mhide - disabled");
    }
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ApplySettings(const Settings *s)
{
    g_cfg = *s;
    SaveSettings();
    if (!g_cfg.enabled)
        ShowSystemCursor();
    ScheduleHide();
    UpdateTrayTooltip();
}

/* --------------------------------------------------------------- tray icon */

static void AddTrayIcon(void)
{
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hIcon;
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    UpdateTrayTooltip();
}

static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(int x, int y)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"&Settings...");
    AppendMenuW(menu, MF_STRING | (g_cfg.enabled ? MF_CHECKED : 0), IDM_ENABLED, L"&Enabled");
    AppendMenuW(menu, MF_STRING, IDM_HIDENOW, L"&Hide cursor now");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);

    /* Required so the menu closes when the user clicks elsewhere. */
    SetForegroundWindow(g_hWnd);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, x, y, g_hWnd, NULL);
    PostMessageW(g_hWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* --------------------------------------------------------- settings dialog */

static void UpdateDialogStatus(HWND hDlg)
{
    WCHAR text[128];
    if (!g_cfg.enabled) {
        StringCchCopyW(text, ARRAYSIZE(text), L"Disabled: the cursor stays visible.");
    } else if (g_hidden) {
        StringCchCopyW(text, ARRAYSIZE(text), L"Cursor is hidden. Move the mouse or press a key to show it.");
    } else {
        DWORD ms = MillisUntilHide();
        StringCchPrintfW(text, ARRAYSIZE(text), L"Cursor is visible. Hiding in %u s.", (ms + 999) / 1000);
    }
    SetDlgItemTextW(hDlg, IDC_STATUS, text);
}

static BOOL ReadDialogSettings(HWND hDlg, Settings *out)
{
    BOOL ok = FALSE;
    UINT delay = GetDlgItemInt(hDlg, IDC_DELAY, &ok, FALSE);
    if (!ok || delay < MIN_DELAY_SECONDS || delay > MAX_DELAY_SECONDS) {
        WCHAR msg[128];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"Delay must be between %u and %u seconds.",
                         MIN_DELAY_SECONDS, MAX_DELAY_SECONDS);
        MessageBoxW(hDlg, msg, APP_NAME, MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(hDlg, IDC_DELAY));
        return FALSE;
    }
    out->delaySeconds = delay;
    out->enabled = IsDlgButtonChecked(hDlg, IDC_ENABLED) == BST_CHECKED ? 1 : 0;
    return TRUE;
}

static BOOL ApplyDialog(HWND hDlg)
{
    Settings s;
    if (!ReadDialogSettings(hDlg, &s)) return FALSE;
    ApplySettings(&s);
    SetAutostart(IsDlgButtonChecked(hDlg, IDC_AUTOSTART) == BST_CHECKED);
    UpdateDialogStatus(hDlg);
    return TRUE;
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (msg) {
    case WM_INITDIALOG:
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)g_hIcon);
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)g_hIcon);
        SendDlgItemMessageW(hDlg, IDC_DELAY_SPIN, UDM_SETRANGE32, MIN_DELAY_SECONDS, MAX_DELAY_SECONDS);
        SendDlgItemMessageW(hDlg, IDC_DELAY, EM_LIMITTEXT, 5, 0);
        SetDlgItemInt(hDlg, IDC_DELAY, g_cfg.delaySeconds, FALSE);
        CheckDlgButton(hDlg, IDC_ENABLED, g_cfg.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_AUTOSTART, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        UpdateDialogStatus(hDlg);
        /* The status line only ticks while the dialog is open. */
        SetTimer(hDlg, TIMER_DLG_STATUS, 500, NULL);
        return TRUE;

    case WM_TIMER:
        if (wParam == TIMER_DLG_STATUS) UpdateDialogStatus(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (ApplyDialog(hDlg)) DestroyWindow(hDlg);
            return TRUE;
        case IDC_APPLY:
            ApplyDialog(hDlg);
            return TRUE;
        case IDCANCEL:
            DestroyWindow(hDlg);
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hDlg, TIMER_DLG_STATUS);
        g_hDlg = NULL;
        return TRUE;
    }
    return FALSE;
}

static void OpenSettingsDialog(void)
{
    if (g_hDlg) {
        if (IsIconic(g_hDlg)) ShowWindow(g_hDlg, SW_RESTORE);
        SetForegroundWindow(g_hDlg);
        return;
    }
    g_hDlg = CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, SettingsDlgProc, 0);
    if (g_hDlg) {
        ShowWindow(g_hDlg, SW_SHOW);
        SetForegroundWindow(g_hDlg);
    }
}

/* ------------------------------------------------------------- main window */

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_hWnd = hWnd;
        AddTrayIcon();
        ScheduleHide();
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_HIDE) {
            ScheduleHide();   /* hides if due, otherwise re-arms for the remaining time */
        } else if (wParam == TIMER_FALLBACK) {
            DWORD t = GetLastInputTick();
            if (t != g_hiddenInputTick) {
                Log("fallback: input tick %lu != %lu", t, g_hiddenInputTick);
                OnActivity();
            }
        }
        return 0;

    case WM_INPUT:
        TraceInput((HRAWINPUT)lParam);
        OnActivity();
        break;  /* DefWindowProc must see WM_INPUT for cleanup */

    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONDBLCLK:
            OpenSettingsDialog();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SETTINGS:
            OpenSettingsDialog();
            break;
        case IDM_ENABLED: {
            Settings s = g_cfg;
            s.enabled = !s.enabled;
            ApplySettings(&s);
            if (g_hDlg) CheckDlgButton(g_hDlg, IDC_ENABLED, s.enabled ? BST_CHECKED : BST_UNCHECKED);
            break;
        }
        case IDM_HIDENOW:
            HideSystemCursor();
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        }
        return 0;

    case WM_OPEN_SETTINGS:
        OpenSettingsDialog();
        return 0;

    case WM_QUERYENDSESSION:
        ShowSystemCursor();
        return TRUE;

    case WM_ENDSESSION:
        if (wParam) ShowSystemCursor();
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_HIDE);
        KillTimer(hWnd, TIMER_FALLBACK);
        if (g_hDlg) DestroyWindow(g_hDlg);
        ShowSystemCursor();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg != 0) {
            /* Explorer restarted: the tray icon has to be re-added. */
            AddTrayIcon();
            return 0;
        }
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
{
    UNREFERENCED_PARAMETER(info);
    /* Never leave the user without a cursor if we die unexpectedly. */
    SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
    HANDLE mutex;
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    HWND hWnd;
    MSG m;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    g_hInst = hInstance;

    /* "mhide --restore" just puts the user's cursor scheme back and exits. */
    if (wcsncmp(lpCmdLine, L"--restore", 9) == 0) {
        SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);
        return 0;
    }
    /* "mhide --watchdog <pid>" is the helper spawned by EnsureWatchdog(). */
    if (wcsncmp(lpCmdLine, L"--watchdog ", 11) == 0)
        return RunWatchdog(wcstoul(lpCmdLine + 11, NULL, 10));

    /* Single instance: if already running, ask it to open its settings dialog. */
    mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(WND_CLASS, NULL);
        if (existing) PostMessageW(existing, WM_OPEN_SETTINGS, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    /* A previous instance may have died while hidden: always start visible. */
    SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);

    OpenLog();
    Log("start pid %lu", GetCurrentProcessId());
    SetUnhandledExceptionFilter(CrashFilter);

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    LoadSettings();

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = g_hIcon;
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    /* A hidden top-level window (not message-only) so Raw Input can target it. */
    hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, WND_CLASS, APP_NAME, WS_POPUP,
                           0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hWnd) return 1;

    /* Release start-up pages we no longer need; the steady-state footprint is tiny. */
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_hDlg && IsDialogMessageW(g_hDlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    ShowSystemCursor();
    if (g_hWatchdog) CloseHandle(g_hWatchdog);
    if (mutex) CloseHandle(mutex);
    return (int)m.wParam;
}
