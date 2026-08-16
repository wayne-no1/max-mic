#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <gdiplus.h>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// --- GUID 用來識別「自己」的音量修改，避免把自己的修改當外部變化 ---
static const GUID MY_GUID = { 0x8d784260, 0x3126, 0x4b6a, { 0x87, 0x57, 0x93, 0xc0, 0x96, 0xd2, 0x68, 0x67 } };

std::atomic<bool>      g_isLocked(true);
std::atomic<float>     g_targetVolume(1.0f);
std::atomic<bool>      g_isDarkMode(true);

// 修正冷卻：防止回調觸發後連續多次寫入
std::atomic<ULONGLONG> g_lastCorrectionTime(0);
const DWORD CORRECTION_COOLDOWN_MS = 300;
std::atomic<bool>      g_needsCorrection(false);
std::atomic<ULONGLONG> g_lastVolumeChangedTime(0);

// 全域 volume 介面指標（監聽執行緒管理生命週期）
IAudioEndpointVolume* g_pVolume = nullptr;

HWND g_hwnd = nullptr;

#define WM_TRAYICON (WM_USER + 1)
#define IDI_TRAYICON 1001
#define IDC_DEVICE_COMBO 2001

NOTIFYICONDATAW g_nid = { 0 };

std::vector<std::wstring> g_deviceIds;
std::wstring g_selectedDeviceId = L"";
std::atomic<bool> g_deviceChanged(false);

void RegisterStartup() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, MAX_PATH)) {
        std::wstring cmdLine = L"\"" + std::wstring(szPath) + L"\" --startup";
        HKEY hKey;
        LONG lResult = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey);
        if (lResult == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"FixMic", 0, REG_SZ, (BYTE*)cmdLine.c_str(), (cmdLine.length() + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }
    }
}

void RefreshDeviceList(HWND hwndCombo) {
    std::wstring prevSelected = g_selectedDeviceId;

    SendMessageW(hwndCombo, CB_RESETCONTENT, 0, 0);
    g_deviceIds.clear();

    SendMessageW(hwndCombo, CB_ADDSTRING, 0, (LPARAM)L"系統預設麥克風");
    g_deviceIds.push_back(L"");

    CoInitialize(nullptr);
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&enumerator))) {
        IMMDeviceCollection* pCollection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection))) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = nullptr;
                if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
                    LPWSTR pwszID = nullptr;
                    if (SUCCEEDED(pDevice->GetId(&pwszID))) {
                        IPropertyStore* pProps = nullptr;
                        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);
                            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                                SendMessageW(hwndCombo, CB_ADDSTRING, 0, (LPARAM)varName.pwszVal);
                                g_deviceIds.push_back(pwszID);
                                PropVariantClear(&varName);
                            }
                            pProps->Release();
                        }
                        CoTaskMemFree(pwszID);
                    }
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
        enumerator->Release();
    }
    CoUninitialize();

    int selIndex = 0;
    for (size_t i = 0; i < g_deviceIds.size(); i++) {
        if (g_deviceIds[i] == prevSelected) {
            selIndex = (int)i;
            break;
        }
    }
    SendMessageW(hwndCombo, CB_SETCURSEL, selIndex, 0);
}

// ============================================================
// IAudioEndpointVolumeCallback 實作
// 每當系統音量變化（不管誰改的）就會被呼叫
// ============================================================
class VolumeCallback : public IAudioEndpointVolumeCallback {
    LONG m_ref;
public:
    VolumeCallback() : m_ref(1) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // 核心回調：音量任何變化都會進來
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override {
        if (!pNotify) return S_OK;

        // 如果這次變化是「我自己」用 MY_GUID 觸發的，忽略，避免無限循環
        if (pNotify->guidEventContext == MY_GUID) return S_OK;

        if (g_isLocked) {
            // 如果開啟了鎖定，檢測到外部變更時，不立刻修正，而是記錄時間，延遲 500ms 後由監聽執行緒修正
            float target = g_targetVolume.load();
            bool volumeChanged = (fabs(pNotify->fMasterVolume - target) > 0.005f);
            bool muteChanged   = (pNotify->bMuted == TRUE);

            if (volumeChanged || muteChanged) {
                g_lastVolumeChangedTime = GetTickCount64();
                g_needsCorrection = true;
            }
        } else {
            // 如果沒有鎖定，當外部調整系統音量時，同步更新我們程式的目標值與 UI 拉條
            g_targetVolume = pNotify->fMasterVolume;
            if (g_hwnd) {
                InvalidateRect(g_hwnd, nullptr, FALSE);
            }
        }
        return S_OK;
    }
};

// ============================================================
// 監聽執行緒：負責建立 COM、註冊 Callback、維持生命週期
// ============================================================
DWORD WINAPI MonitoringThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&enumerator);

    VolumeCallback* pCallback = nullptr;

    auto cleanupDevice = [&]() {
        if (pCallback && g_pVolume) {
            g_pVolume->UnregisterControlChangeNotify(pCallback);
            pCallback->Release();
            pCallback = nullptr;
        }
        if (g_pVolume) {
            g_pVolume->Release();
            g_pVolume = nullptr;
        }
    };

    while (true) {
        IMMDevice* device = nullptr;
        HRESULT hr = E_FAIL;
        
        // 根據選定的設備 ID 獲取設備，空代表預設裝置
        if (g_selectedDeviceId.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        } else {
            hr = enumerator->GetDevice(g_selectedDeviceId.c_str(), &device);
        }

        if (SUCCEEDED(hr)) {
            IAudioEndpointVolume* vol = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol))) {
                g_pVolume = vol;

                pCallback = new VolumeCallback();
                g_pVolume->RegisterControlChangeNotify(pCallback);

                // 通知 UI 重繪以更新音量拉條位置
                if (g_hwnd) {
                    InvalidateRect(g_hwnd, nullptr, FALSE);
                }

                // 如果已鎖定，立刻套用音量
                if (g_isLocked) {
                    g_pVolume->SetMasterVolumeLevelScalar(g_targetVolume, &MY_GUID);
                    g_pVolume->SetMute(FALSE, &MY_GUID);
                }

                g_deviceChanged = false;

                // 檢查迴圈：定期偵測裝置狀態或切換通知（改用較小 sleep 間隔以提供精準的 0.5 秒延遲鎖定）
                int checkCount = 0;
                while (true) {
                    Sleep(50);

                    // 檢查是否需要執行延遲的音量修正
                    if (g_needsCorrection.load()) {
                        ULONGLONG now = GetTickCount64();
                        if (now - g_lastVolumeChangedTime.load() >= 500) {
                            g_needsCorrection = false;
                            if (g_isLocked && g_pVolume) {
                                g_pVolume->SetMasterVolumeLevelScalar(g_targetVolume, &MY_GUID);
                                g_pVolume->SetMute(FALSE, &MY_GUID);
                            }
                        }
                    }

                    // 檢查 UI 是否切換了裝置
                    if (g_deviceChanged.load()) {
                        break;
                    }

                    // 每隔 40 次 (50ms * 40 = 2 秒) 檢查一次當前選取設備是否還有效
                    if (++checkCount >= 40) {
                        checkCount = 0;
                        IMMDevice* test = nullptr;
                        HRESULT testHr = E_FAIL;
                        if (g_selectedDeviceId.empty()) {
                            testHr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &test);
                        } else {
                            testHr = enumerator->GetDevice(g_selectedDeviceId.c_str(), &test);
                        }
                        if (FAILED(testHr)) {
                            // 裝置失效（例如被拔除），跳出重連
                            break;
                        }
                        if (test) test->Release();
                    }
                }

                cleanupDevice();
            }
            device->Release();
        }
        Sleep(1000); // 失敗或重連前的等待時間
    }

    cleanupDevice();
    if (enumerator) enumerator->Release();
    CoUninitialize();
    return 0;
}

// ============================================================
// GDI+ 繪製輔助
// ============================================================
struct Theme {
    Color bg, card, text, accent, secondary;
};
Theme LightTheme = { Color(255,242,242,247), Color(255,255,255,255), Color(255,0,0,0),   Color(255,0,122,255), Color(255,142,142,147) };
Theme DarkTheme  = { Color(255,0,0,0),       Color(255,28,28,30),   Color(255,255,255,255), Color(255,10,132,255), Color(255,72,72,74) };

void FillRoundedRect(Graphics& g, Brush* brush, RectF rect, float radius) {
    GraphicsPath path;
    path.AddArc(rect.X,                       rect.Y,                        radius, radius, 180, 90);
    path.AddArc(rect.X + rect.Width - radius, rect.Y,                        radius, radius, 270, 90);
    path.AddArc(rect.X + rect.Width - radius, rect.Y + rect.Height - radius, radius, radius,   0, 90);
    path.AddArc(rect.X,                       rect.Y + rect.Height - radius, radius, radius,  90, 90);
    path.CloseFigure();
    g.FillPath(brush, &path);
}

// ============================================================
// 視窗訊息處理
// ============================================================
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    Theme& cur = g_isDarkMode ? DarkTheme : LightTheme;
    
    switch (uMsg) {
    case WM_CREATE: {
        // 建立 ComboBox 下拉選單 (X=20, Y=90, W=260, H=200)
        HWND hwndCombo = CreateWindowExW(
            0, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            20, 90, 260, 200,
            hwnd, (HMENU)IDC_DEVICE_COMBO,
            ((LPCREATESTRUCT)lParam)->hInstance, nullptr
        );
        
        // 設定選單字型
        HFONT hFont = CreateFontW(15, 0, 0, 0, FW_REGULAR, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft JhengHei");
        SendMessageW(hwndCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 初始化列舉裝置
        RefreshDeviceList(hwndCombo);
        return 0;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);
        if (wmId == IDC_DEVICE_COMBO) {
            if (wmEvent == CBN_DROPDOWN) {
                // 每次下拉展開時，動態刷新裝置清單，支援即時插拔
                RefreshDeviceList((HWND)lParam);
            } else if (wmEvent == CBN_SELCHANGE) {
                int sel = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)g_deviceIds.size()) {
                    g_selectedDeviceId = g_deviceIds[sel];
                    g_deviceChanged = true; // 通知背景執行緒重新連線新裝置
                }
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        SelectObject(hdcMem, hbm);

        Graphics g(hdcMem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(cur.bg);

        FontFamily ff(L"Segoe UI");
        Font titleFont(&ff, 20, FontStyleBold,    UnitPixel);
        Font labelFont(&ff, 15, FontStyleRegular, UnitPixel);

        SolidBrush textBrush(cur.text);
        SolidBrush secBrush(cur.secondary);
        SolidBrush accentBrush(cur.accent);
        SolidBrush cardBrush(cur.card);
        SolidBrush whiteBrush(Color(255,255,255,255));
        Pen shadowPen(Color(40,0,0,0), 1);

        // 標題
        g.DrawString(L"Microphone Lock", -1, &titleFont, PointF(25, 25), &textBrush);

        // 麥克風裝置標籤
        g.DrawString(L"Microphone Device", -1, &labelFont, PointF(25, 70), &textBrush);

        // 音量卡片 (移動到 Y=145)
        FillRoundedRect(g, &cardBrush, RectF(20, 145, 260, 100), 24);
        std::wstring volStr = L"Volume: " + std::to_wstring((int)(g_targetVolume * 100)) + L"%";
        g.DrawString(volStr.c_str(), -1, &labelFont, PointF(40, 165), &textBrush);

        // 滑桿 (移動到 Y=200)
        FillRoundedRect(g, &secBrush,   RectF(40, 200, 220, 10), 10);
        FillRoundedRect(g, &accentBrush, RectF(40, 200, 220 * g_targetVolume, 10), 10);
        RectF thumbRect(40 + (220 * g_targetVolume) - 13, 205 - 13, 26, 26);
        g.FillEllipse(&whiteBrush, thumbRect);
        g.DrawEllipse(&shadowPen,  thumbRect);

        // 鎖定開關 (移動到 Y=270)
        g.DrawString(L"Auto Lock Volume", -1, &labelFont, PointF(25, 275), &textBrush);
        RectF toggleRect(210, 270, 52, 28);
        FillRoundedRect(g, g_isLocked ? &accentBrush : &secBrush, toggleRect, 28);
        RectF circleRect(g_isLocked ? 210+26 : 210+2, 270+2, 24, 24);
        g.FillEllipse(&whiteBrush, circleRect);

        // 主題切換按鈕 (移動到 Y=320)
        FillRoundedRect(g, &cardBrush, RectF(20, 320, 260, 44), 22);
        Font emojiFont(L"Segoe UI Emoji", 15, FontStyleRegular, UnitPixel);
        std::wstring modeStr = g_isDarkMode ? L"\x2600  Switch to Light Mode" : L"\x23FE  Switch to Dark Mode";
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(modeStr.c_str(), -1, &emojiFont, RectF(20,320,260,44), &sf, &textBrush);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        // 滑桿 (移動到 Y=200，偵測範圍設為 Y=185 ~ 225)
        if (x >= 40 && x <= 260 && y >= 185 && y <= 225) {
            float v = (float)(x - 40) / 220.0f;
            if (v < 0) v = 0; if (v > 1) v = 1;
            g_targetVolume = v;
            // 立即寫入音量
            if (g_pVolume) {
                g_pVolume->SetMasterVolumeLevelScalar(v, &MY_GUID);
                g_pVolume->SetMute(FALSE, &MY_GUID);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            SetCapture(hwnd);
        }
        // 鎖定開關 (移動到 Y=270，偵測範圍設為 Y=270 ~ 298)
        if (x >= 210 && x <= 262 && y >= 270 && y <= 298) {
            g_isLocked = !g_isLocked;
            // 開啟鎖定時立即套用目標音量
            if (g_isLocked && g_pVolume) {
                g_pVolume->SetMasterVolumeLevelScalar(g_targetVolume, &MY_GUID);
                g_pVolume->SetMute(FALSE, &MY_GUID);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // 主題按鈕 (移動到 Y=320，偵測範圍設為 Y=320 ~ 364)
        if (x >= 20 && x <= 280 && y >= 320 && y <= 364) {
            g_isDarkMode = !g_isDarkMode;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if ((wParam & MK_LBUTTON) && GetCapture() == hwnd) {
            int x = LOWORD(lParam);
            float v = (float)(x - 40) / 220.0f;
            if (v < 0) v = 0; if (v > 1) v = 1;
            g_targetVolume = v;
            if (g_pVolume) {
                g_pVolume->SetMasterVolumeLevelScalar(v, &MY_GUID);
                g_pVolume->SetMute(FALSE, &MY_GUID);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"開啟主畫面");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 2, L"結束程式");

            POINT pt;
            GetCursorPos(&pt);

            SetForegroundWindow(hwnd);
            int id = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            if (id == 1) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (id == 2) {
                Shell_NotifyIconW(NIM_DELETE, &g_nid);
                DestroyWindow(hwnd);
            }
            DestroyMenu(hMenu);
        }
        return 0;
    }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ============================================================
// 入口點
// ============================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // 單一執行個體檢查 (Single Instance Check)
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\FixMicMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hwndExisting = FindWindowW(L"iOSFixMic", L"FixMic");
        if (hwndExisting) {
            ShowWindow(hwndExisting, SW_SHOW);
            SetForegroundWindow(hwndExisting);
        }
        CloseHandle(hMutex);
        return 0;
    }

    // 自動註冊開機自啟動
    RegisterStartup();

    // 解析命令列參數
    bool startMinimized = false;
    int nArgs;
    LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (szArglist != nullptr) {
        for (int i = 1; i < nArgs; i++) {
            if (wcscmp(szArglist[i], L"--startup") == 0) {
                startMinimized = true;
                break;
            }
        }
        LocalFree(szArglist);
    }

    GdiplusStartupInput gsi;
    ULONG_PTR gToken;
    GdiplusStartup(&gToken, &gsi, nullptr);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"iOSFixMic";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = ExtractIconW(hInstance, L"shell32.dll", 140);
    RegisterClassW(&wc);

    RECT rc = {0, 0, 300, 385};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hwnd = CreateWindowW(L"iOSFixMic", L"FixMic",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    // 註冊系統工作列圖示 (Tray Icon)
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = IDI_TRAYICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = ExtractIconW(hInstance, L"shell32.dll", 140);
    wcscpy_s(g_nid.szTip, L"FixMic - 麥克風音量鎖定");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (startMinimized) {
        ShowWindow(g_hwnd, SW_HIDE);
    } else {
        ShowWindow(g_hwnd, nCmdShow);
    }

    CreateThread(nullptr, 0, MonitoringThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gToken);
    CloseHandle(hMutex);
    return 0;
}   