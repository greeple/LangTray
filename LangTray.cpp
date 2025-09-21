// LangTray.cpp
// Трей-приложение: меняет иконку по текущей раскладке клавиатуры.
// Совместимо с Windows XP ... Windows 11.
//
// Сборка (MSVC):
//   cl /nologo /O2 /utf-8 LangTray.cpp /DUNICODE /D_UNICODE /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib gdi32.lib advapi32.lib
// Сборка (MinGW):
//   g++ -O2 -municode -mwindows -finput-charset=UTF-8 LangTray.cpp -o LangTray.exe -luser32 -lshell32 -lgdi32 -ladvapi32
//
// Иконки: .\icons\*.ico (см. правила в LoadIconForLang)

#define UNICODE
#define _UNICODE
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define _WIN32_IE 0x0600

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

// Настройки
static const UINT  TIMER_ID       = 1;
static const UINT  TIMER_INTERVAL = 1000; // мс — резервный таймер
static const UINT  TRAY_ICON_ID   = 1;
static const UINT  WM_TRAYICON    = WM_APP + 1;

#define IDM_OPEN_ICONS 1001
#define IDM_EXIT       1002

HINSTANCE g_hInst = nullptr;
HICON     g_hIconCurrent = nullptr;
LANGID    g_lastLang = 0;
HWND      g_hWnd = nullptr;
WCHAR     g_exeDir[MAX_PATH] = {0};
WCHAR     g_iconsDir[MAX_PATH] = {0};
UINT      WM_TASKBARCREATED = 0;
UINT      WM_SHELLHOOK = 0;

static BOOL FileExistsW_(LPCWSTR path)
{
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void GetExeDir()
{
    DWORD len = GetModuleFileNameW(nullptr, g_exeDir, MAX_PATH);
    if (len > 0) {
        for (int i = (int)len - 1; i >= 0; --i) {
            if (g_exeDir[i] == L'\\' || g_exeDir[i] == L'/') {
                g_exeDir[i] = 0;
                break;
            }
        }
    } else {
        lstrcpyW(g_exeDir, L".");
    }
    wsprintfW(g_iconsDir, L"%s\\icons", g_exeDir);
}

static void ToHex4(WCHAR out[5], WORD v)
{
    const WCHAR* hex = L"0123456789ABCDEF";
    out[0] = hex[(v >> 12) & 0xF];
    out[1] = hex[(v >> 8) & 0xF];
    out[2] = hex[(v >> 4) & 0xF];
    out[3] = hex[v & 0xF];
    out[4] = 0;
}

static void GetIsoCodesFromLangId(LANGID lang, WCHAR isoLang[16], WCHAR isoCtry[16])
{
    LCID lcid = MAKELCID(lang, SORT_DEFAULT);
    isoLang[0] = isoCtry[0] = 0;
    GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, isoLang, 16);
    GetLocaleInfoW(lcid, LOCALE_SISO3166CTRYNAME, isoCtry, 16);
}

static void GetEnglishNamesForTooltip(LANGID lang, WCHAR buf[128])
{
    LCID lcid = MAKELCID(lang, SORT_DEFAULT);
    WCHAR langName[64] = L"", ctryName[64] = L"";
    GetLocaleInfoW(lcid, LOCALE_SENGLANGUAGE, langName, 64);
    GetLocaleInfoW(lcid, LOCALE_SENGCOUNTRY, ctryName, 64);
    if (ctryName[0]) {
        wsprintfW(buf, L"%s (%s)", langName, ctryName);
    } else {
        lstrcpynW(buf, langName, 128);
    }
}

static HICON LoadIconFromFile(LPCWSTR path)
{
    return (HICON)LoadImageW(nullptr, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
}

// Выбирает .ico по языку. Порядок приоритетов:
//   icons\<lang>-<country>.ico (en-US.ico)
//   icons\<lang>_<country>.ico (en_US.ico)
//   icons\<lang>.ico (en.ico)
//   icons\<LANGID-hex>.ico (0409.ico)
//   icons\default.ico
static HICON LoadIconForLang(LANGID lang)
{
    WCHAR isoLang[16], isoCtry[16];
    GetIsoCodesFromLangId(lang, isoLang, isoCtry);

    WCHAR candidates[6][MAX_PATH];
    int idx = 0;

    if (isoLang[0] && isoCtry[0]) {
        wsprintfW(candidates[idx++], L"%s\\%s-%s.ico", g_iconsDir, isoLang, isoCtry);
        wsprintfW(candidates[idx++], L"%s\\%s_%s.ico", g_iconsDir, isoLang, isoCtry);
    }
    if (isoLang[0]) {
        wsprintfW(candidates[idx++], L"%s\\%s.ico", g_iconsDir, isoLang);
    }
    WCHAR hex4[5]; ToHex4(hex4, lang);
    wsprintfW(candidates[idx++], L"%s\\%s.ico", g_iconsDir, hex4);
    wsprintfW(candidates[idx++], L"%s\\default.ico", g_iconsDir);

    for (int i = 0; i < idx; ++i) {
        if (FileExistsW_(candidates[i])) {
            HICON h = LoadIconFromFile(candidates[i]);
            if (h) return h;
        }
    }
    return (HICON)LoadIconW(nullptr, IDI_APPLICATION);
}

static UINT GetNidCbSize()
{
#ifdef NOTIFYICONDATA_V2_SIZE
    return NOTIFYICONDATA_V2_SIZE; // XP понимает v2
#else
    return sizeof(NOTIFYICONDATA);
#endif
}

static void TrayAddOrUpdate(BOOL add)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = GetNidCbSize();
    nid.hWnd = g_hWnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_hIconCurrent ? g_hIconCurrent : (HICON)LoadIconW(nullptr, IDI_APPLICATION);

    WCHAR tip[128] = L"";
    if (g_lastLang) GetEnglishNamesForTooltip(g_lastLang, tip);
    if (!tip[0]) lstrcpyW(tip, L"Language tray");
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));

    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
    nid.uVersion = NOTIFYICON_VERSION; // широкая совместимость (XP+)
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void TrayDelete()
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = GetNidCbSize();
    nid.hWnd = g_hWnd;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void UpdateIconIfChanged(LANGID newLang)
{
    if (newLang == 0) return;

    if (g_lastLang != newLang || g_hIconCurrent == nullptr) {
        g_lastLang = newLang;
        HICON hNew = LoadIconForLang(newLang);
        if (hNew) {
            if (g_hIconCurrent && g_hIconCurrent != hNew) {
                DestroyIcon(g_hIconCurrent);
            }
            g_hIconCurrent = hNew;
            TrayAddOrUpdate(FALSE); // NIM_MODIFY
        }
    }
}

static LANGID QueryForegroundLang()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (!tid) return 0;
    HKL hkl = GetKeyboardLayout(tid);
    return (LANGID)(UINT_PTR)hkl; // LOWORD(HKL)
}

static void ShowTrayMenu()
{
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // Если кириллица отображается неверно — см. комментарии вверху (UTF-8 + /utf-8).
    AppendMenuW(menu, MF_STRING, IDM_OPEN_ICONS, L"Открыть папку с иконками");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Выход");

    SetForegroundWindow(g_hWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hWnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hWnd, WM_NULL, 0, 0);
}

static void OpenIconsFolder()
{
    ShellExecuteW(nullptr, L"open", g_iconsDir, nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Explorer перезапущен — заново добавить иконку
    if (msg == WM_TASKBARCREATED) {
        TrayAddOrUpdate(TRUE);
        return 0;
    }

    // Обработка shell hook сообщений (мгновенная реакция)
    if (msg == WM_SHELLHOOK) {
        // Изменение языка или активация окна — обновим иконку
        if (wParam == HSHELL_LANGUAGE || wParam == HSHELL_WINDOWACTIVATED
#ifdef HSHELL_RUDEAPPACTIVATED
            || wParam == HSHELL_RUDEAPPACTIVATED
#endif
            ) {
            LANGID lang = QueryForegroundLang();
            UpdateIconIfChanged(lang);
        }
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
        g_hWnd = hWnd;

        // Регистрируем окно как получатель shell hook
        RegisterShellHookWindow(hWnd);

        // Резервный таймер (на случай если hook не сработал)
        SetTimer(hWnd, TIMER_ID, TIMER_INTERVAL, nullptr);

        // Начальная иконка по текущей раскладке
        UpdateIconIfChanged(QueryForegroundLang());
        TrayAddOrUpdate(TRUE);
        return 0;

    case WM_INPUTLANGCHANGE:
        // lParam = HKL, LOWORD -> LANGID
        UpdateIconIfChanged((LANGID)(UINT_PTR)lParam);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID) {
            UpdateIconIfChanged(QueryForegroundLang());
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowTrayMenu();
        } else if (lParam == WM_LBUTTONDBLCLK) {
            OpenIconsFolder();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN_ICONS: OpenIconsFolder(); break;
        case IDM_EXIT: DestroyWindow(hWnd); break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID);
        TrayDelete();
        if (g_hIconCurrent) {
            DestroyIcon(g_hIconCurrent);
            g_hIconCurrent = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInstance;
    GetExeDir();
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    WM_SHELLHOOK      = RegisterWindowMessageW(L"SHELLHOOK");

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = L"LangTrayHiddenWindow";
    wc.hIconSm       = wc.hIcon;

    if (!RegisterClassExW(&wc)) return 0;

    HWND hWnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                wc.lpszClassName, L"LangTray",
                                WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return 0;

    // Скрытое окно — просто цикл сообщений
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
