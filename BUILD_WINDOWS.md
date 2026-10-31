# Build instructions for Windows

## Prerequisites
- CMake
- Visual Studio or MinGW-w64
- (Optional) ncurses for Unix-like builds

## Build with MinGW
```bash
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
```

## Build with Visual Studio
```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## Run
```bash
ebook_reader.exe path\to\book.txt
```
