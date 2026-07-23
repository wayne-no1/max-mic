# FixMic

A lightweight Windows utility to lock your microphone volume, prevent unwanted muting, and support dynamic device selection.

## Key Features
- **Volume Lock & Anti-Mute**: Reverts external microphone volume changes after a **0.5s delay** (debounce mechanism) and instantly unmutes.
- **Device Selector**: Dynamically select which microphone device in your system to lock (supports real-time hot-plugging).
- **Run in Background**: Minimize/hide to the system tray on close (`X` button). Double-click tray icon to open; right-click to exit.
- **Auto Startup**: Registers to Windows Registry (`HKCU`) on boot. Runs silently in tray when launched with `--startup`.
- **Theme**: Defaulted to Dark Mode (toggle at the bottom).

## How to Build (MinGW)
```bash
g++ -o max-mic max-mic.cpp -lgdiplus -lole32 -luuid -lcomctl32 -mwindows
```

## How to Use
1. Run `max-mic.exe`.
2. Select your microphone from the **Microphone Device** dropdown at the top.
3. Adjust the slider to set your target volume.
4. Toggle **Auto Lock Volume** to enable/disable locking.
5. Click **`X`** to hide it to the system tray. To fully close, right-click the tray icon and select **çµ????ç¨?å¼? (Exit)**.
