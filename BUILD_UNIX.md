# Build instructions for Linux/macOS

## Prerequisites
- CMake
- ncurses development library

### Ubuntu/Debian
```bash
sudo apt-get install cmake libncurses-dev g++
```

### Fedora
```bash
sudo dnf install cmake ncurses-devel gcc-c++
```

### macOS (with Homebrew)
```bash
brew install cmake ncurses
```

## Build
```bash
mkdir build
cd build
cmake ..
make
```

## Run
```bash
./ebook_reader path/to/book.txt
```
