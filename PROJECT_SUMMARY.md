# Cross-platform TUI Ebook Reader

A fully functional command-line ebook reader with TUI interface supporting multiple formats.

## Features
- **Cross-platform TUI**: Native Windows console API on Windows, ncurses on Linux/macOS
- **Multiple formats**: TXT (fully supported), EPUB/DOCX/PDF (framework in place, need additional libraries for full parsing)
- **Smooth navigation**: Scroll up/down, page up/down, jump to top/bottom
- **Table of contents**: Support for chapter navigation (when available)
- **Help system**: Built-in help with keybindings
- **UTF-8 support**: Handles UTF-8 encoded text files

## Quick Start

### Windows
```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
.\ebook_reader.exe ..\test.txt
```

### Linux/macOS
```bash
mkdir build && cd build
cmake ..
make
./ebook_reader ../test.txt
```

## Controls
| Key | Action |
|---|---|
| `j` or `↓` | Scroll down |
| `k` or `↑` | Scroll up |
| `f` or `PgDn` | Page down |
| `b` or `PgUp` | Page up |
| `g` or `Home` | Go to top |
| `G` or `End` | Go to bottom |
| `t` | Table of contents |
| `h` or `?` | Help |
| `q` or `ESC` | Quit |

## Project Structure
```
ebook_reader/
├── CMakeLists.txt
├── README.md
├── BUILD_WINDOWS.md
├── BUILD_UNIX.md
├── include/
│   ├── tui.h
│   ├── reader.h
│   └── parser.h
└── src/
    ├── main.cpp
    ├── reader.cpp
    ├── parser_txt.cpp
    ├── parser_epub.cpp
    ├── parser_docx.cpp
    ├── parser_pdf.cpp
    ├── tui_windows.cpp
    └── tui_ncurses.cpp
```

## Notes
- TXT format is fully functional
- EPUB/DOCX/PDF parsers are stubs that need external libraries (minizip/libzip for ZIP-based formats, Poppler/MuPDF for PDF)
- The TUI uses platform-specific backends for maximum compatibility
