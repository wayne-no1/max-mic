# max-mic

Windows C++ tool to lock microphone volume at 100% and prevent muting.

## Features
- **Auto-Restore**: Fixes volume/mute changes instantly.
- **Background**: Runs silently without a console.
- **Efficient**: Minimal resource usage.

## Setup & Build
- **OS**: Windows 10/11
- **Build**: `g++ -o max-mic max-mic.cpp -lole32 -luuid`

## Usage
Run `./max-mic`. The process hides itself and runs in the background. Use Task Manager to stop.

## License
MIT
