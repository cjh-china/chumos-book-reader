#include "tui.h"
#include <windows.h>
#include <conio.h>
#include <iostream>
#include <cstdlib>

namespace {

int utf8Len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

} // namespace

TUI::TUI() : initialized(false) {}
TUI::~TUI() {
    if (initialized) shutdown();
}

bool TUI::init() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hOut, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hOut, &cursorInfo);
    SetConsoleOutputCP(65001);

    // 关闭 QuickEdit：否则鼠标点到窗口会进入选择模式，键盘输入被冻结
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hIn, &mode)) {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, mode);
    }

    initialized = true;
    return true;
}

void TUI::shutdown() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hOut, &cursorInfo);
    cursorInfo.bVisible = TRUE;
    SetConsoleCursorInfo(hOut, &cursorInfo);
}

void TUI::clear() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;

    DWORD cellCount = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    DWORD written = 0;
    COORD home = {0, 0};

    FillConsoleOutputCharacterA(hOut, ' ', cellCount, home, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, cellCount, home, &written);
    SetConsoleCursorPosition(hOut, home);
}

void TUI::refresh() {
    std::cout.flush();
}

int TUI::getHeight() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (h > 0) return h;
    }
    // 输出被重定向或无控制台时的安全回退
    const char* env = std::getenv("LINES");
    if (env) { int v = std::atoi(env); if (v > 0) return v; }
    return 24;
}

int TUI::getWidth() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (w > 0) return w;
    }
    const char* env = std::getenv("COLUMNS");
    if (env) { int v = std::atoi(env); if (v > 0) return v; }
    return 80;
}

void TUI::drawText(int row, int col, const std::string& text) {
    COORD coord = {(SHORT)col, (SHORT)row};
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
    std::cout << text;
}

void TUI::drawLine(int row, int col, const std::string& text, int maxWidth) {
    COORD coord = {(SHORT)col, (SHORT)row};
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
    std::string display;
    int w = 0;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = (unsigned char)text[i];
        int len = utf8Len(c);
        if (i + len > text.size()) len = 1;
        unsigned int cp = 0;
        if (len == 1) cp = c;
        else if (len == 2) cp = c & 0x1F;
        else if (len == 3) cp = c & 0x0F;
        else cp = c & 0x07;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
        bool wide = (cp >= 0x1100 && (
            cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
            (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x20000 && cp <= 0x3FFFD)));
        int cw = wide ? 2 : 1;
        if (w + cw > maxWidth) break;
        display.append(text, i, len);
        w += cw;
        i += len;
    }
    std::cout << display;
}

int TUI::getKey() {
    if (_kbhit()) {
        int key = _getch();
        if (key == 0 || key == 224) {
            int key2 = _getch();
            if (key2 == 72) return 1000;
            if (key2 == 80) return 1001;
            if (key2 == 73) return 1002;
            if (key2 == 81) return 1003;
            if (key2 == 71) return 1004;
            if (key2 == 79) return 1005;
            if (key2 == 75) return 1006;  // 左
            if (key2 == 77) return 1007;  // 右
        }
        return key;
    }
    return -1;
}

void TUI::showStatus(const std::string& status) {
    int height = getHeight();
    int width = getWidth();
    COORD coord = {0, (SHORT)(height - 1)};
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
    std::cout << std::string(width, ' ');
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
    std::cout << status;
}

void TUI::setCursorVisible(bool visible) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci;
    GetConsoleCursorInfo(hOut, &ci);
    ci.bVisible = visible ? TRUE : FALSE;
    SetConsoleCursorInfo(hOut, &ci);
}

std::string TUI::promptInput(int row, int col, const std::string& prompt, const std::string& initial) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    setCursorVisible(true);
    SetConsoleCursorPosition(hOut, {(SHORT)col, (SHORT)row});
    std::cout << prompt;
    if (!initial.empty()) std::cout << initial;
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line)) line.clear();

    setCursorVisible(false);
    return line;
}
