#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <gdiplus.h>
#include <atomic>
#include <cmath>
#include <string>

// Link necessary libraries
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// --- Core Logic & Constants ---
static const GUID MY_GUID = { 0x8d784260, 0x3126, 0x4b6a, { 0x87, 0x57, 0x93, 0xc0, 0x96, 0xd2, 0x68, 0x67 } };
std::atomic<bool> g_isLocked(true);
std::atomic<float> g_targetVolume(1.0f);
std::atomic<bool> g_needsCorrection(false);
std::atomic<ULONGLONG> g_lastChangeTime(0);
std::atomic<bool> g_isDarkMode(false);
const DWORD DEBOUNCE_TIME_MS = 500;

// --- iOS Theme Colors ---
struct Theme {
    Color bg;
    Color card;
    Color text;
    Color accent;
    Color secondary;
};

Theme LightTheme = { Color(255, 242, 242, 247), Color(255, 255, 255, 255), Color(255, 0, 0, 0), Color(255, 0, 122, 255), Color(255, 142, 142, 147) };
Theme DarkTheme = { Color(255, 0, 0, 0), Color(255, 28, 28, 30), Color(255, 255, 255, 255), Color(255, 10, 132, 255), Color(255, 72, 72, 74) };

// --- Helper Functions ---
void FillRoundedRect(Graphics& g, Brush* brush, RectF rect, float radius) {
    GraphicsPath path;
    path.AddArc(rect.X, rect.Y, radius, radius, 180, 90);
    path.AddArc(rect.X + rect.Width - radius, rect.Y, radius, radius, 270, 90);
    path.AddArc(rect.X + rect.Width - radius, rect.Y + rect.Height - radius, radius, radius, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - radius, radius, radius, 90, 90);
    path.CloseFigure();
    g.FillPath(brush, &path);
}

// --- Monitoring Thread ---
DWORD WINAPI MonitoringThread(LPVOID lpParam) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = NULL;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);

    while (true) {
        IMMDevice* device = NULL;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) {
            IAudioEndpointVolume* volume = NULL;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&volume))) {
                ULONGLONG lastPollTime = GetTickCount64();
                while (true) {
                    ULONGLONG now = GetTickCount64();
                    if (g_isLocked && g_needsCorrection) {
                        if (now - g_lastChangeTime >= DEBOUNCE_TIME_MS) {
                            volume->SetMasterVolumeLevelScalar(g_targetVolume, &MY_GUID);
                            volume->SetMute(FALSE, &MY_GUID);
                            g_needsCorrection = false;
                        }
                    }
                    if (now - lastPollTime >= 1000) {
                        float vol; BOOL mute;
                        volume->GetMasterVolumeLevelScalar(&vol);
                        volume->GetMute(&mute);
                        if (g_isLocked && (fabs(vol - g_targetVolume) > 0.005f || mute) && !g_needsCorrection) {
                            g_lastChangeTime = now;
                            g_needsCorrection = true;
                        }
                        IMMDevice* test = NULL;
                        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &test))) break;
                        test->Release();
                        lastPollTime = now;
                    }
                    Sleep(50);
                }
                volume->Release();
            }
            device->Release();
        }
        Sleep(2000);
    }
    if (enumerator) enumerator->Release();
    CoUninitialize();
    return 0;
}

// --- Window Procedure ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    Theme& cur = g_isDarkMode ? DarkTheme : LightTheme;

    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            SelectObject(hdcMem, hbmMem);

            Graphics g(hdcMem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.Clear(cur.bg);

            FontFamily fontFamily(L"Segoe UI");
            Font titleFont(&fontFamily, 20, FontStyleBold, UnitPixel);
            Font labelFont(&fontFamily, 15, FontStyleRegular, UnitPixel);
            SolidBrush textBrush(cur.text);
            SolidBrush secBrush(cur.secondary);
            SolidBrush accentBrush(cur.accent);
            SolidBrush cardBrush(cur.card);
            SolidBrush whiteBrush(Color(255, 255, 255, 255));
            Pen shadowPen(Color(40, 0, 0, 0), 1);

            // Title
            g.DrawString(L"Microphone Lock", -1, &titleFont, PointF(25, 25), &textBrush);

            // Volume Card
            RectF cardRect(20, 75, 260, 100);
            FillRoundedRect(g, &cardBrush, cardRect, 24);
            
            std::wstring volStr = L"Volume: " + std::to_wstring((int)(g_targetVolume * 100)) + L"%";
            g.DrawString(volStr.c_str(), -1, &labelFont, PointF(40, 95), &textBrush);

            // iOS Slider Track
            RectF trackRect(40, 130, 220, 10);
            FillRoundedRect(g, &secBrush, trackRect, 10);
            RectF activeRect(40, 130, 220 * g_targetVolume, 10);
            FillRoundedRect(g, &accentBrush, activeRect, 10);
            
            // Slider Thumb
            RectF thumbRect(40 + (220 * g_targetVolume) - 13, 135 - 13, 26, 26);
            g.FillEllipse(&whiteBrush, thumbRect);
            g.DrawEllipse(&shadowPen, thumbRect);

            // Lock Toggle Row
            g.DrawString(L"Auto Lock Volume", -1, &labelFont, PointF(25, 205), &textBrush);
            RectF toggleRect(210, 200, 52, 28);
            FillRoundedRect(g, g_isLocked ? &accentBrush : &secBrush, toggleRect, 28);
            RectF circleRect(g_isLocked ? 210 + 26 : 210 + 2, 200 + 2, 24, 24);
            g.FillEllipse(&whiteBrush, circleRect);

            // Theme Button
            RectF btnRect(20, 250, 260, 44);
            FillRoundedRect(g, &cardBrush, btnRect, 22);
            
            // Use Segoe UI Emoji font for icons
            Font emojiFont(L"Segoe UI Emoji", 15, FontStyleRegular, UnitPixel);
            
            // Use hex codes: \x2600 is Sun, \x23FE is Moon (better support in standard fonts)
            std::wstring modeStr = g_isDarkMode ? L"\x2600  Switch to Light Mode" : L"\x23FE  Switch to Dark Mode";
            
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(modeStr.c_str(), -1, &emojiFont, btnRect, &format, &textBrush);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            if (x >= 40 && x <= 260 && y >= 115 && y <= 155) {
                g_targetVolume = (float)(x - 40) / 220.0f;
                g_needsCorrection = true;
                InvalidateRect(hwnd, NULL, FALSE);
                SetCapture(hwnd);
            }
            if (x >= 210 && x <= 262 && y >= 200 && y <= 228) {
                g_isLocked = !g_isLocked;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (x >= 20 && x <= 280 && y >= 250 && y <= 294) {
                g_isDarkMode = !g_isDarkMode;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (wParam & MK_LBUTTON && GetCapture() == hwnd) {
                int x = LOWORD(lParam);
                float val = (float)(x - 40) / 220.0f;
                if (val < 0) val = 0; if (val > 1) val = 1;
                g_targetVolume = val;
                g_needsCorrection = true;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            ReleaseCapture();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    GdiplusStartupInput gsi;
    ULONG_PTR gToken;
    GdiplusStartup(&gToken, &gsi, NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"iOSFixMic";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // Load Microphone icon from system shell32.dll (usually index 140)
    wc.hIcon = ExtractIconW(hInstance, L"shell32.dll", 140);
    RegisterClassW(&wc);

    RECT rc = { 0, 0, 300, 320 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

    HWND hwnd = CreateWindowW(L"iOSFixMic", L"FixMic", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, 
        NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);
    CreateThread(NULL, 0, MonitoringThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gToken);
    return 0;
}
