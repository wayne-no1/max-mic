# FixMic

A modern Windows C++ utility to lock your microphone volume and prevent unwanted muting. Featuring a clean, iOS-inspired user interface.

## Features
- **Volume Locking**: Keeps your microphone at a specific volume level automatically.
- **Anti-Mute**: Instantly unmutes the microphone if it's muted by other applications.
- **iOS-style GUI**: A beautiful, minimalist interface with smooth sliders and toggles.
- **Dark Mode Support**: Toggle between Light and Dark themes.
- **Low Resource Usage**: Efficient background monitoring with minimal CPU impact.

## Setup & Build
- **OS**: Windows 10/11
- **Compiler**: MinGW-w64 (g++) or MSVC
- **Build Command (MinGW)**:
  ```bash
  g++ -o fix-mic max-mic.cpp -lgdiplus -lole32 -luuid -lcomctl32 -mwindows
  ```

## Usage
1. Type `.\max-mic.exe` into the terminal.
2. Use the slider to set your desired microphone volume.
3. Toggle "Auto Lock Volume" to enable/disable the enforcement.
4. Switch between Light and Dark modes using the bottom button.

## License
MIT
