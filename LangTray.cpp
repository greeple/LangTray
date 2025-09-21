// LangTray.cpp
// Трей-приложение: меняет иконку по текущей раскладке клавиатуры.
// Windows XP ... Windows 11.
//
// Сборка (MSVC):
//   cl /nologo /O2 /utf-8 LangTray.cpp /DUNICODE /D_UNICODE /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib gdi32.lib advapi32.lib
// Сборка (MinGW):
//   g++ -O2 -municode -mwindows -finput-charset=UTF-8 LangTray.cpp -o LangTray.exe -luser32 -lshell32 -lgdi32 -ladvapi32

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

// Трей/меню
static const UINT  TRAY_ICON_ID   = 1;
static const UINT  WM_TRAYICON    = WM_APP + 1;

// Одноразовый таймер-«debounce» после горячих клавиш (не опрос!)
static const UINT  TIMER_DEBOUNCE_ID = 2;
static const UINT  TIMER_DEBOUNCE_MS = 40;

#define IDM_OPEN_ICONS 1001
#define IDM_EXIT       1002

HINSTANCE g_hInst = nullptr;
HWND      g_hWnd = nullptr;
HICON     g_hIconCurrent = nullptr;
LANGID    g_lastLang = 0;

WCHAR     g_exeDir[MAX_PATH] = {0};
WCHAR     g_iconsDir[MAX_PATH] = {0};
BOOL      g_hasIconsDir = FALSE;

UINT      WM_TASKBARCREATED = 0;
UINT      WM_SHELLHOOK      = 0;

// Низкоуровневый хук клавиатуры
HHOOK     g_hHookLL = nullptr;
BOOL      g_pendingUpdate = FALSE;
BOOL      g_alt = FALSE, g_ctrl = FALSE, g_shift = FALSE, g_win = FALSE;

static BOOL FileExistsW_(LPCWSTR path)
{
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}
static BOOL DirExistsW_(LPCWSTR path)
{
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
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
    g_hasIconsDir = DirExistsW_(g_iconsDir);
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

static HICON TryLoadInIcons(LPCWSTR leaf)
{
    if (!g_hasIconsDir) return nullptr;
    WCHAR path[MAX_PATH];
    wsprintfW(path, L"%s\\%s", g_iconsDir, leaf);
    if (FileExistsW_(path)) {
        if (HICON h = LoadIconFromFile(path)) return h;
    }
    return nullptr;
}

// Порядок поиска (пример для 1033/en-US):
//  - 1033.ico (десятичный LANGID — основной вариант, как в паках Punto)
//  - en-US.ico, en_US.ico, en.ico (fallback)
//  - 0409.ico (hex LANGID — fallback)
//  - default.ico (fallback)
static HICON LoadIconForLang(LANGID lang)
{
    WCHAR dec[16]; wsprintfW(dec, L"%u", (UINT)lang);
    WCHAR isoLang[16] = L"", isoCtry[16] = L"";
    GetIsoCodesFromLangId(lang, isoLang, isoCtry);
    WCHAR hex4[5]; ToHex4(hex4, lang);

    // 1033.ico
    WCHAR decIco[32]; wsprintfW(decIco, L"%s.ico", dec);
    if (HICON h = TryLoadInIcons(decIco)) return h;

    // ISO-варианты
    if (isoLang[0] && isoCtry[0]) {
        WCHAR isoDash[64]; wsprintfW(isoDash, L"%s-%s.ico", isoLang, isoCtry);
        if (HICON h = TryLoadInIcons(isoDash)) return h;
        WCHAR isoUnd[64];  wsprintfW(isoUnd,  L"%s_%s.ico", isoLang, isoCtry);
        if (HICON h = TryLoadInIcons(isoUnd)) return h;
    }
    if (isoLang[0]) {
        WCHAR isoOnly[32]; wsprintfW(isoOnly, L"%s.ico", isoLang);
        if (HICON h = TryLoadInIcons(isoOnly)) return h;
    }

    // 0409.ico (hex)
    WCHAR hexIco[32]; wsprintfW(hexIco, L"%s.ico", hex4);
    if (HICON h = TryLoadInIcons(hexIco)) return h;

    // default.ico
    if (HICON h = TryLoadInIcons(L"default.ico")) return h;

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
    nid.uVersion = NOTIFYICON_VERSION; // совместимо с XP+
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

static LANGID QueryForegroundLang()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (!tid) return 0;
    HKL hkl = GetKeyboardLayout(tid);
    return (LANGID)(UINT_PTR)hkl; // LOWORD(HKL)
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

static void ScheduleDebouncedUpdate(UINT delayMs)
{
    // Одноразово — без опроса
    if (!g_pendingUpdate && g_hWnd) {
        g_pendingUpdate = TRUE;
        SetTimer(g_hWnd, TIMER_DEBOUNCE_ID, delayMs, nullptr);
    }
}

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
        const DWORD vk = k->vkCode;
        const BOOL down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const BOOL up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        // Обновляем модификаторы
        auto setMod = [&](DWORD vkey, BOOL isDown){
            switch (vkey) {
                case VK_LMENU: case VK_RMENU:    g_alt   = isDown ? TRUE : (up ? (vk==vkey?FALSE:g_alt) : g_alt); break;
                case VK_LCONTROL: case VK_RCONTROL: g_ctrl  = isDown ? TRUE : (up ? (vk==vkey?FALSE:g_ctrl) : g_ctrl); break;
                case VK_LSHIFT: case VK_RSHIFT:  g_shift = isDown ? TRUE : (up ? (vk==vkey?FALSE:g_shift) : g_shift); break;
                case VK_LWIN: case VK_RWIN:      g_win   = isDown ? TRUE : (up ? (vk==vkey?FALSE:g_win) : g_win); break;
            }
        };

        // Сначала установим состояние при нажатии
        if (down) {
            setMod(vk, TRUE);
        }

        // Детектируем типичные хоткеи смены раскладки:
        // Alt+Shift, Ctrl+Shift, Win+Space
        BOOL trigger = FALSE;
        if (down) {
            if ((vk == VK_LSHIFT || vk == VK_RSHIFT) && (g_alt || g_ctrl)) trigger = TRUE;
            if ((vk == VK_LMENU  || vk == VK_RMENU)  && g_shift)           trigger = TRUE;
            if ((vk == VK_LCONTROL || vk == VK_RCONTROL) && g_shift)       trigger = TRUE;
            if (vk == VK_SPACE && g_win)                                   trigger = TRUE;
        }

        // Планируем одноразовое обновление (после применения раскладки системой)
        if (trigger) {
            ScheduleDebouncedUpdate(TIMER_DEBOUNCE_MS);
        }

        // Обновим состояние при отпускании
        if (up) {
            setMod(vk, FALSE);
        }
    }
    return CallNextHookEx(g_hHookLL, code, wParam, lParam);
}

static void ShowTrayMenu()
{
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

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
    if (g_hasIconsDir) {
        ShellExecuteW(nullptr, L"open", g_iconsDir, nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", g_exeDir, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TASKBARCREATED) {
        TrayAddOrUpdate(TRUE);
        return 0;
    }

    if (msg == WM_SHELLHOOK) {
        // Реагируем на активацию окна и (если ОС пошлёт) — на смену языка
        if (wParam == HSHELL_WINDOWACTIVATED
#ifdef HSHELL_RUDEAPPACTIVATED
            || wParam == HSHELL_RUDEAPPACTIVATED
#endif
            || wParam == HSHELL_LANGUAGE) {
            UpdateIconIfChanged(QueryForegroundLang());
        }
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
        g_hWnd = hWnd;

        // Shell hook: смена фокуса/редкие события смены языка
        RegisterShellHookWindow(hWnd);

        // Низкоуровневый хук клавиатуры: ловим горячие клавиши смены раскладки
        g_hHookLL = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);

        // Стартовая иконка
        UpdateIconIfChanged(QueryForegroundLang());
        TrayAddOrUpdate(TRUE);
        return 0;

    case WM_INPUTLANGCHANGE:
        // Обычно мы это не получим (сообщение идёт активному окну), но на всякий случай:
        UpdateIconIfChanged((LANGID)(UINT_PTR)lParam);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DEBOUNCE_ID) {
            KillTimer(hWnd, TIMER_DEBOUNCE_ID);
            g_pendingUpdate = FALSE;
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
        if (g_hHookLL) { UnhookWindowsHookEx(g_hHookLL); g_hHookLL = nullptr; }
        TrayDelete();
        if (g_hIconCurrent) { DestroyIcon(g_hIconCurrent); g_hIconCurrent = nullptr; }
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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
