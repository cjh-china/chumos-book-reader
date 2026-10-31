#include "tui.h"
#include <ncurses.h>
#include <cstring>
#include <clocale>

TUI::TUI() : initialized(false) {}
TUI::~TUI() {
    if (initialized) shutdown();
}

bool TUI::init() {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    initialized = true;
    return true;
}

void TUI::shutdown() {
    endwin();
    initialized = false;
}

void TUI::clear() {
    ::clear();
}

void TUI::refresh() {
    ::refresh();
}

int TUI::getHeight() {
    return LINES;
}

int TUI::getWidth() {
    return COLS;
}

void TUI::drawText(int row, int col, const std::string& text) {
    mvprintw(row, col, "%s", text.c_str());
}

void TUI::drawLine(int row, int col, const std::string& text, int maxWidth) {
    std::string display = text;
    if ((int)display.length() > maxWidth) {
        display = display.substr(0, maxWidth);
    }
    mvprintw(row, col, "%s", display.c_str());
}

int TUI::getKey() {
    return getch();
}

void TUI::showStatus(const std::string& status) {
    int height = getHeight();
    int width = getWidth();
    move(height - 1, 0);
    for (int i = 0; i < width; i++) printw(" ");
    move(height - 1, 0);
    printw("%s", status.c_str());
    ::refresh();
}

void TUI::setCursorVisible(bool visible) {
    curs_set(visible ? 1 : 0);
}

std::string TUI::promptInput(int row, int col, const std::string& prompt, const std::string& initial) {
    curs_set(1);
    echo();
    move(row, col);
    clrtoeol();
    mvprintw(row, col, "%s", prompt.c_str());
    if (!initial.empty()) printw("%s", initial.c_str());
    ::refresh();
    char buf[512];
    memset(buf, 0, sizeof(buf));
    getnstr(buf, sizeof(buf) - 1);
    noecho();
    curs_set(0);
    return std::string(buf);
}
