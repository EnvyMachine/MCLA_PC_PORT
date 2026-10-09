#include "window.h"
#include "../runtime/log.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace render
{
    namespace
    {
        std::atomic<uint32_t> s_width, s_height;
        std::atomic<HWND> s_hwnd{ nullptr };
        bool s_fullscreen = false;
        WINDOWPLACEMENT s_windowed{ sizeof(WINDOWPLACEMENT) };

        constexpr UINT_PTR kCursorTimer = 1, kBarAnimTimer = 2, kBarHideTimer = 3;
        constexpr UINT kSysMenuFullscreen = 0x1F10; // system menu item (low 4 bits clear, below 0xF000)

        void ToggleFullscreen(HWND hwnd);

        // ---- Mouse cursor: hidden over the game after a moment without movement ----

        POINT s_lastMouse{ -1, -1 };
        ULONGLONG s_lastMouseMove = 0;
        bool s_cursorHidden = false;

        ULONGLONG CursorHideDelayMs()
        {
            // MCLA_CURSOR_HIDE_SECONDS=<s>: how long an idle cursor stays (0 = never hide).
            static const ULONGLONG delay = [] {
                char v[16];
                return GetEnvironmentVariableA("MCLA_CURSOR_HIDE_SECONDS", v, sizeof(v)) > 0 ? ULONGLONG(atof(v) * 1000.0)
                                                                                                : 2000ull;
            }();
            return delay;
        }

        void ShowCursorAgain()
        {
            s_lastMouseMove = GetTickCount64();
            if (s_cursorHidden)
            {
                s_cursorHidden = false;
                SetCursor(LoadCursorA(nullptr, IDC_ARROW));
            }
        }

        // Called every 250 ms: hides the cursor once it has rested over the game's
        // client area (in the foreground) for the delay.
        void CheckIdleCursor(HWND hwnd)
        {
            ULONGLONG delay = CursorHideDelayMs();
            if (s_cursorHidden || !delay || GetTickCount64() - s_lastMouseMove < delay || GetForegroundWindow() != hwnd)
                return;
            POINT pt;
            if (!GetCursorPos(&pt) || WindowFromPoint(pt) != hwnd)
                return;
            ScreenToClient(hwnd, &pt);
            RECT client;
            GetClientRect(hwnd, &client);
            if (!PtInRect(&client, pt))
                return;
            s_cursorHidden = true;
            SetCursor(nullptr);
        }

        // ---- Fullscreen bar, like Remote Desktop's connection bar ----
        // Pointing at the top middle of the screen in fullscreen slides it down with
        // the title and minimize / exit full screen / close buttons.

        HWND s_bar = nullptr;
        bool s_barShown = false;   // target of the slide
        int s_barOffset = 0;       // pixels pushed up off the screen (bar height = hidden)
        int s_barHover = -1;       // button under the cursor
        ULONGLONG s_barLeftAt = 0; // when the cursor left the bar
        constexpr int kBarWidth = 470, kBarHeight = 36; // at 96 DPI
        enum BarButton { kBarMinimize, kBarExitFullscreen, kBarClose, kBarButtonCount };

        float Scale()
        {
            HWND hwnd = s_hwnd.load();
            return hwnd ? float(GetDpiForWindow(hwnd)) / 96.0f : 1.0f;
        }

        int Px(float v) { return int(v * Scale() + 0.5f); }

        // The bar's rectangle on screen when fully shown (top middle of the game's monitor).
        RECT BarScreenRect()
        {
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(s_hwnd.load(), MONITOR_DEFAULTTOPRIMARY), &mi);
            int w = Px(kBarWidth), h = Px(kBarHeight);
            int x = (mi.rcMonitor.left + mi.rcMonitor.right - w) / 2;
            return { x, mi.rcMonitor.top, x + w, mi.rcMonitor.top + h };
        }

        // Buttons from the right edge: close, exit full screen (with its label), minimize.
        RECT BarButtonRect(int button)
        {
            int w = Px(kBarWidth), h = Px(kBarHeight), icon = Px(46), wide = Px(150);
            switch (button)
            {
            case kBarClose: return { w - icon, 0, w, h };
            case kBarExitFullscreen: return { w - icon - wide, 0, w - icon, h };
            default: return { w - icon - wide - icon, 0, w - icon - wide, h };
            }
        }

        void PlaceBar()
        {
            RECT r = BarScreenRect();
            SetWindowPos(s_bar, nullptr, r.left, r.top - s_barOffset, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }

        void DrawBar(HDC mem, int w, int h)
        {
            RECT client{ 0, 0, w, h };
            HBRUSH background = CreateSolidBrush(RGB(32, 32, 36));
            FillRect(mem, &client, background);
            DeleteObject(background);
            RECT accent{ 0, h - Px(2), w, h };
            HBRUSH accentBrush = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(mem, &accent, accentBrush);
            DeleteObject(accentBrush);

            for (int b = 0; b < kBarButtonCount; b++)
            {
                if (b != s_barHover)
                    continue;
                RECT r = BarButtonRect(b);
                r.bottom -= Px(2);
                HBRUSH hover = CreateSolidBrush(b == kBarClose ? RGB(196, 43, 28) : RGB(62, 62, 68));
                FillRect(mem, &r, hover);
                DeleteObject(hover);
            }

            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(240, 240, 240));
            HFONT text = CreateFontW(-Px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT icons = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
            HGDIOBJ oldFont = SelectObject(mem, text);
            RECT title{ Px(14), 0, BarButtonRect(kBarMinimize).left, h - Px(2) };
            DrawTextW(mem, L"Midnight Club: Los Angeles", -1, &title, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

            // Minimize and close: Segoe MDL2 glyphs; exit full screen: glyph and label.
            SelectObject(mem, icons);
            RECT r = BarButtonRect(kBarMinimize);
            r.bottom -= Px(2);
            DrawTextW(mem, L"\xE921", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            r = BarButtonRect(kBarClose);
            r.bottom -= Px(2);
            DrawTextW(mem, L"\xE8BB", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            r = BarButtonRect(kBarExitFullscreen);
            r.bottom -= Px(2);
            RECT glyph{ r.left + Px(12), r.top, r.left + Px(32), r.bottom };
            DrawTextW(mem, L"\xE73F", -1, &glyph, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            SelectObject(mem, text);
            RECT label{ glyph.right + Px(6), r.top, r.right, r.bottom };
            DrawTextW(mem, L"Exit full screen", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            SelectObject(mem, oldFont);
            DeleteObject(text);
            DeleteObject(icons);
        }

        void PaintBar(HWND hwnd)
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
            DrawBar(mem, client.right, client.bottom);
            BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
        }

        // MCLA_TEST_BAR=<file.bmp>: draws the bar (with "Exit full screen" hovered) into
        // that file at startup, to check its look without going fullscreen.
        void DumpBarImage()
        {
            char path[MAX_PATH];
            if (GetEnvironmentVariableA("MCLA_TEST_BAR", path, sizeof(path)) == 0)
                return;
            int w = Px(kBarWidth), h = Px(kBarHeight);
            BITMAPINFO bi{};
            bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
            void* bits = nullptr;
            HDC screen = GetDC(nullptr);
            HDC mem = CreateCompatibleDC(screen);
            HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(mem, dib);
            int hover = s_barHover;
            s_barHover = kBarExitFullscreen;
            DrawBar(mem, w, h);
            s_barHover = hover;
            GdiFlush();
            if (FILE* f = fopen(path, "wb"))
            {
                uint32_t imageBytes = uint32_t(w) * h * 4;
                BITMAPFILEHEADER fh{ 0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageBytes), 0, 0,
                                     DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) };
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
                fwrite(bits, imageBytes, 1, f);
                fclose(f);
            }
            SelectObject(mem, old);
            DeleteObject(dib);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
        }

        void SlideBar(bool show)
        {
            if (!s_bar || s_barShown == show)
                return;
            s_barShown = show;
            if (show)
            {
                s_barLeftAt = 0;
                PlaceBar();
                ShowWindow(s_bar, SW_SHOWNOACTIVATE);
                SetTimer(s_bar, kBarHideTimer, 200, nullptr);
            }
            SetTimer(s_bar, kBarAnimTimer, 15, nullptr);
        }

        void HideBarNow()
        {
            if (!s_bar)
                return;
            s_barShown = false;
            s_barOffset = Px(kBarHeight);
            s_barHover = -1;
            KillTimer(s_bar, kBarAnimTimer);
            KillTimer(s_bar, kBarHideTimer);
            ShowWindow(s_bar, SW_HIDE);
        }

        // The top middle of the screen, where pointing brings the bar down.
        bool InBarHotZone(POINT screen)
        {
            RECT r = BarScreenRect();
            return screen.y <= r.top + Px(3) && screen.x >= r.left && screen.x < r.right;
        }

        LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE; // keep the game focused (controller and keyboard input)
            case WM_PAINT:
                PaintBar(hwnd);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_MOUSEMOVE:
            {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                int hover = -1;
                for (int b = 0; b < kBarButtonCount; b++)
                {
                    RECT r = BarButtonRect(b);
                    if (PtInRect(&r, pt))
                        hover = b;
                }
                if (hover != s_barHover)
                {
                    s_barHover = hover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (s_barHover != -1)
                {
                    s_barHover = -1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP:
            {
                HWND game = s_hwnd.load();
                int button = s_barHover;
                if (button == kBarMinimize)
                {
                    HideBarNow();
                    ShowWindow(game, SW_MINIMIZE);
                }
                else if (button == kBarExitFullscreen)
                {
                    ToggleFullscreen(game);
                }
                else if (button == kBarClose)
                {
                    PostMessageW(game, WM_CLOSE, 0, 0);
                }
                return 0;
            }
            case WM_TIMER:
                if (wParam == kBarAnimTimer)
                {
                    int height = Px(kBarHeight), step = std::max(1, height / 5);
                    s_barOffset = s_barShown ? std::max(0, s_barOffset - step) : std::min(height, s_barOffset + step);
                    PlaceBar();
                    if (s_barShown ? s_barOffset == 0 : s_barOffset == height)
                    {
                        KillTimer(hwnd, kBarAnimTimer);
                        if (!s_barShown)
                        {
                            KillTimer(hwnd, kBarHideTimer);
                            ShowWindow(hwnd, SW_HIDE);
                        }
                    }
                }
                else if (wParam == kBarHideTimer && s_barShown)
                {
                    // Slide away once the cursor has been off the bar for a second.
                    POINT pt;
                    GetCursorPos(&pt);
                    RECT r = BarScreenRect();
                    if (PtInRect(&r, pt) || InBarHotZone(pt))
                        s_barLeftAt = 0;
                    else if (!s_barLeftAt)
                        s_barLeftAt = GetTickCount64();
                    else if (GetTickCount64() - s_barLeftAt > 1000)
                        SlideBar(false);
                }
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorA(nullptr, IDC_ARROW));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        void CreateBar(HWND owner)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = BarProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
            wc.lpszClassName = L"MCLAFullscreenBar";
            RegisterClassExW(&wc);
            // Owned by the game window: stays above it and hides with it.
            s_bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, owner,
                                    nullptr, wc.hInstance, nullptr);
            s_barOffset = Px(kBarHeight);
        }

        // Borderless fullscreen on the window's monitor (Alt+Enter, F11, the window
        // menu's "Full screen"; the bar's "Exit full screen" leaves it).
        void ToggleFullscreen(HWND hwnd)
        {
            if (!s_fullscreen)
            {
                MONITORINFO mi{ sizeof(mi) };
                if (!GetWindowPlacement(hwnd, &s_windowed) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
                    return;
                SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                s_fullscreen = true;
            }
            else
            {
                HideBarNow();
                SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                SetWindowPlacement(hwnd, &s_windowed);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                s_fullscreen = false;
            }
            ShowCursorAgain();
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            switch (msg)
            {
            case WM_SYSKEYDOWN:
                if (wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN))
                {
                    ToggleFullscreen(hwnd);
                    return 0;
                }
                break;
            case WM_KEYDOWN:
                if (wParam == VK_F11)
                {
                    ToggleFullscreen(hwnd);
                    return 0;
                }
                break;
            case WM_SYSCOMMAND:
                // Right-clicking the title bar (the window menu) offers "Full screen".
                if ((wParam & 0xFFF0) == kSysMenuFullscreen)
                {
                    ToggleFullscreen(hwnd);
                    return 0;
                }
                break;
            case WM_INITMENUPOPUP:
                if (HIWORD(lParam)) // the window menu: check "Full screen" while in it
                    CheckMenuItem(GetSystemMenu(hwnd, FALSE), kSysMenuFullscreen,
                                  MF_BYCOMMAND | (s_fullscreen ? MF_CHECKED : MF_UNCHECKED));
                break;
            case WM_MOUSEMOVE:
            {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                // Windows also sends WM_MOUSEMOVE when nothing moved; only real movement counts.
                if (pt.x != s_lastMouse.x || pt.y != s_lastMouse.y)
                {
                    s_lastMouse = pt;
                    ShowCursorAgain();
                    if (s_fullscreen)
                    {
                        POINT screen = pt;
                        ClientToScreen(hwnd, &screen);
                        if (InBarHotZone(screen))
                            SlideBar(true);
                    }
                }
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT && s_cursorHidden)
                {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            case WM_TIMER:
                if (wParam == kCursorTimer)
                    CheckIdleCursor(hwnd);
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE)
                    HideBarNow();
                break;
            case WM_DPICHANGED:
                if (!s_fullscreen)
                {
                    const RECT* r = reinterpret_cast<const RECT*>(lParam);
                    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return 0;
            case WM_SIZE:
                if (wParam != SIZE_MINIMIZED)
                {
                    s_width = LOWORD(lParam);
                    s_height = HIWORD(lParam);
                }
                return 0;
            case WM_CLOSE:
                LOGI("window closed, exiting");
                LogFlush();
                ExitProcess(0);
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    HWND CreateGameWindow(uint32_t width, uint32_t height)
    {
        std::mutex lock;
        std::condition_variable created;
        HWND hwnd = nullptr;
        bool done = false;

        // Render at the display's real resolution (no blurry DPI scaling); the
        // windowed size follows the system scale factor.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        UINT dpi = GetDpiForSystem();
        width = MulDiv(width, dpi, 96);
        height = MulDiv(height, dpi, 96);

        std::thread([&, width, height] {
            SetThreadDescription(GetCurrentThread(), L"MCLA window");
            HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
            // The program icon (res/mcla.rc), at the sizes the title bar and taskbar use.
            UINT dpi = GetDpiForSystem();
            wc.hIcon = HICON(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, dpi),
                                        GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
            wc.hIconSm = HICON(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                          GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0));
            wc.lpszClassName = L"MCLAWindow";
            RegisterClassExW(&wc);

            RECT rect{ 0, 0, LONG(width), LONG(height) };
            DWORD style = WS_OVERLAPPEDWINDOW;
            AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());
            HWND w = CreateWindowExW(0, wc.lpszClassName, L"Midnight Club: Los Angeles", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                     rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
            RECT client{};
            GetClientRect(w, &client);
            s_width = uint32_t(client.right - client.left);
            s_height = uint32_t(client.bottom - client.top);
            s_hwnd = w;

            // "Full screen" in the window menu (right-click the title bar), after Maximize.
            if (HMENU menu = GetSystemMenu(w, FALSE))
                InsertMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_STRING, kSysMenuFullscreen, L"&Full screen\tF11");
            CreateBar(w);
            DumpBarImage();
            s_lastMouseMove = GetTickCount64();
            SetTimer(w, kCursorTimer, 250, nullptr);

            ShowWindow(w, SW_SHOW);
            if (GetEnvironmentVariableA("MCLA_FULLSCREEN", nullptr, 0) > 0 || strstr(GetCommandLineA(), "--fullscreen"))
                ToggleFullscreen(w);
            {
                std::lock_guard g(lock);
                hwnd = w;
                done = true;
            }
            created.notify_one();

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }).detach();

        std::unique_lock g(lock);
        created.wait(g, [&] { return done; });
        return hwnd;
    }

    void GetClientSize(uint32_t& width, uint32_t& height)
    {
        width = s_width.load();
        height = s_height.load();
    }

    HWND GameWindow() { return s_hwnd.load(); }
}