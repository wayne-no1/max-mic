#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <iostream>
#include <atomic>

std::atomic<bool> g_needsCorrection(false);
std::atomic<ULONGLONG> g_lastChangeTime(0);
const DWORD DEBOUNCE_TIME_MS = 300; 

class VolumeCallback : public IAudioEndpointVolumeCallback {
    LONG _refCount;
    IAudioEndpointVolume* _volume;

public:
    VolumeCallback(IAudioEndpointVolume* volume) : _refCount(1), _volume(volume) {
        _volume->AddRef();
    }
    ~VolumeCallback() { _volume->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef(); return S_OK;
        }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&_refCount); }
    STDMETHODIMP_(ULONG) Release() {
        ULONG res = InterlockedDecrement(&_refCount);
        if (res == 0) delete this;
        return res;
    }

    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) {
        if (pNotify->fMasterVolume < 0.999f || pNotify->bMuted) {
            g_lastChangeTime = GetTickCount64();

            if (!g_needsCorrection) {
                printf("[Notify] Volume changed, starting 0.3s countdown...\n");
                g_needsCorrection = true;
            }
        } else if (pNotify->fMasterVolume >= 0.999f && !pNotify->bMuted) {
            if (g_needsCorrection) {
                printf("[Cancel] Volume manually restored, stopping countdown.\n");
                g_needsCorrection = false;
            }
        }
        return S_OK;
    }
};

void StartMonitoring() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = NULL;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);

    while (true) {
        IMMDevice* device = NULL;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) {
            IAudioEndpointVolume* volume = NULL;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&volume))) {
                
                volume->SetMasterVolumeLevelScalar(1.0f, NULL);
                volume->SetMute(FALSE, NULL);
                printf("[System] Monitoring started, locked at 100%%\n");

                VolumeCallback* callback = new VolumeCallback(volume);
                volume->RegisterControlChangeNotify(callback);

                ULONGLONG lastPollTime = GetTickCount64();

                while (true) {
                    ULONGLONG now = GetTickCount64();

                    if (g_needsCorrection) {
                        ULONGLONG elapsed = now - g_lastChangeTime;
                        if (elapsed >= DEBOUNCE_TIME_MS) {
                            volume->SetMasterVolumeLevelScalar(1.0f, NULL);
                            volume->SetMute(FALSE, NULL);
                            printf("[Action] Idle for 0.3s, auto-restoring to 100%%!\n");
                            g_needsCorrection = false;
                        }
                    }

                    if (now - lastPollTime >= 2000) {
                        float vol; BOOL mute;
                        volume->GetMasterVolumeLevelScalar(&vol);
                        volume->GetMute(&mute);
                        if ((vol < 0.999f || mute) && !g_needsCorrection) {
                            g_lastChangeTime = now;
                            g_needsCorrection = true;
                            printf("[Guard] Background drift detected, starting correction...\n");
                        }

                        IMMDevice* test = NULL;
                        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &test))) break;
                        test->Release();
                        lastPollTime = now;
                    }

                    Sleep(10); 
                }

                volume->UnregisterControlChangeNotify(callback);
                callback->Release();
                volume->Release();
            }
            device->Release();
        }
        Sleep(3000);
    }
    enumerator->Release();
    CoUninitialize();
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("========================================\n");
    printf("   max-mic (Microphone Volume Guardian)\n");
    printf("========================================\n");
    
    FreeConsole();

    StartMonitoring();
    return 0;
}
