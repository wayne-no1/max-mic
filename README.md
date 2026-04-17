## Setup & Build
- **OS**: Windows 10/11
- **Compiler**: MinGW-w64 (g++) or MSVC
- **Build Command (MinGW)**:
  ```bash
  g++ -o fix-mic max-mic.cpp -lgdiplus -lole32 -luuid -lcomctl32 -mwindows
  ```

## Usage
Type `.\max-mic.exe` into the terminal.

## License
MIT
