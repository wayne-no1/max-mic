# FixMic

A modern Windows C++ utility to lock your microphone volume, select specific microphone devices, and prevent unwanted muting. Featuring a clean, iOS-inspired user interface.

## Features
- **Device Selection Dropdown**: Dynamically lists and lets you select any active capture device (microphone) in your system. It supports hot-plugging; simply click the dropdown to refresh the list of devices in real-time.
- **Bi-directional Volume Sync**:
  - Automatically retrieves the actual microphone volume level upon launch to initialize the slider.
  - When **Auto Lock Volume** is disabled, adjusting the volume from the Windows settings or other apps will dynamically synchronize and move the slider in FixMic.
- **Delayed Volume Locking (0.5s)**: When locking is enabled, external volume modifications are reverted back to the target lock volume after a 0.5-second delay. This debounce mechanism ensures you can adjust volume smoothly without instant friction while dragging.
- **Anti-Mute**: Instantly unmutes the microphone if it's muted by other applications.
- **System Tray Integration**: Closing the window (`X` button) hides it to the system tray (notification area). Double-click the tray icon to restore the window, or right-click to access the menu.
- **Boot Startup Registration**: Automatically registers to the Windows Registry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`) on startup. It boots silently in the background (hidden in the system tray) when started with the `--startup` parameter.
- **Single Instance Mutex**: Ensures only one instance of FixMic runs. Running it again will automatically bring the existing background window to the foreground.
- **iOS-style GUI**: A beautiful, minimalist interface with smooth custom-drawn GDI+ sliders and toggles, defaulted to **Dark Mode**.

## Setup & Build
- **OS**: Windows 10/11
- **Compiler**: MinGW-w64 (g++) or MSVC
- **Build Command (MinGW)**:
  ```bash
  g++ -o max-mic max-mic.cpp -lgdiplus -lole32 -luuid -lcomctl32 -mwindows
  ```

## Usage
1. Compile and run `max-mic.exe`.
2. Select your target microphone from the **Microphone Device** dropdown at the top.
3. Use the slider to set your desired microphone volume.
4. Toggle **Auto Lock Volume** to enable/disable the enforcement.
5. Switch between Dark and Light themes using the bottom button.
6. Click the close (`X`) button to run it in the background. Right-click the tray icon and select **結束程式 (Exit)** to fully close the application.

## License
MIT
