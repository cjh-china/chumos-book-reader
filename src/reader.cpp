#include "reader.h"
#include "parser.h"
#include "site_parser.h"
#include "fs_util.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>
#include <ctime>
#include <chrono>

#ifdef PLATFORM_WINDOWS
#include <conio.h>
#include <windows.h>
#else
#include <ncurses.h>
#endif

namespace {
std::string trimStrLocal(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
}

Reader::Reader(TUI* tuiPtr) : tui(tuiPtr), currentLine(0), running(true), padding(2), matchIndex(0) {
    // row 0 = 标题，row 1..height-2 = 正文，row height-1 = 状态栏
    linesPerPage = tui->getHeight() - 2;
    if (linesPerPage < 5) linesPerPage = 5;
}

Reader::~Reader() {
}

void Reader::resetBook() {
    book.title.clear();
    book.author.clear();
    book.content.clear();
    book.chapters.clear();
    book.chapterStarts.clear();
    book.wrappedContent.clear();

    currentLine = 0;
    lastQuery.clear();
    matchLines.clear();
    matchIndex = 0;
}

bool Reader::loadBook(const std::string& filename) {
    // 必须在解析前清空，否则新书内容会追加到旧书后面
    resetBook();

    if (filename.find("http://") == 0 || filename.find("https://") == 0) {
#ifdef HAS_CURL
        if (!SiteParser::parseUrl(filename, book)) { resetBook(); return false; }
        buildWrappedContent();
        cfg.recordFile(filename);
        return true;
#else
        std::cerr << "在线阅读需要 libcurl 支持" << std::endl;
        return false;
#endif
    }

    if (!Parser::parse(filename, book)) {
        resetBook();
        return false;
    }
    buildWrappedContent();
    cfg.recordFile(filename);
    return true;
}

void Reader::buildWrappedContent() {
    book.wrappedContent.clear();
    int width = tui->getWidth() - (padding * 2);
    if (width < 20) width = 20;

    // 判断码点是否为宽字符（占 2 列）
    auto isWide = [](unsigned int cp) {
        return cp >= 0x1100 && (
            cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
            (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x20000 && cp <= 0x3FFFD));
    };

    for (const auto& line : book.content) {
        if (line.empty()) {
            book.wrappedContent.push_back("");
            continue;
        }

        // 章节标题行不折行（与 Parser 用同一套判定）
        if (Parser::isChapterTitle(line)) {
            book.wrappedContent.push_back(line);
            continue;
        }

        // 按 UTF-8 字符边界累加，按显示列宽换行
        std::string current;
        int used = 0;
        size_t i = 0;

        while (i < line.size()) {
            unsigned char c = (unsigned char)line[i];
            int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            if (i + (size_t)len > line.size()) len = 1;

            unsigned int cp = 0;
            if (len == 1) cp = c;
            else if (len == 2) cp = c & 0x1F;
            else if (len == 3) cp = c & 0x0F;
            else cp = c & 0x07;
            for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)line[i + k] & 0x3F);

            int cw = isWide(cp) ? 2 : 1;

            if (used + cw > width && used > 0) {
                book.wrappedContent.push_back(current);
                current.clear();
                used = 0;
            }

            current.append(line, i, (size_t)len);
            used += cw;
            i += (size_t)len;
        }

        if (!current.empty()) book.wrappedContent.push_back(current);
    }
}

void Reader::run(bool manageTui) {
    if (manageTui) {
        if (!tui->init()) return;
    }

    // 进入阅读时清一次屏，避免书库/搜索界面残留
    tui->clear();

    buildWrappedContent();
    linesPerPage = tui->getHeight() - 2;
    if (linesPerPage < 5) linesPerPage = 5;

    running = true;
    returnToLibrary = false;
    bool needRedraw = true;

    while (running) {
        if (needRedraw) {
            display();
            needRedraw = false;
        }

        int key = tui->getKey();
        if (key != -1) {
            handleInput(key);
            needRedraw = true;
        } else {
#ifdef PLATFORM_WINDOWS
            Sleep(20);
#else
            napms(20);
#endif
        }
    }

    if (manageTui) tui->shutdown();
}

int Reader::getCurrentChapterIndex() const {
    if (book.chapters.empty()) return -1;
    return 0;
}

void Reader::display() {
    int height = tui->getHeight();
    int width = tui->getWidth();

    // 正文可用行数：row 0 标题，row height-1 状态栏
    int bodyRows = height - 2;
    if (bodyRows < 1) bodyRows = 1;
    if (linesPerPage > bodyRows) linesPerPage = bodyRows;

    // 逐行整行覆盖：先拼出 width 宽度的整行（含前导空格），一次定位写入
    auto putLine = [&](int row, const std::string& text, int col) {
        if (col < 0) col = 0;
        if (col >= width) col = width - 1;

        std::string line((size_t)col, ' ');
        line += text;

        // 按显示宽度补齐/截断到 width
        int w = 0;
        size_t i = 0;
        std::string fitted;
        fitted.reserve(line.size());
        while (i < line.size()) {
            unsigned char c = (unsigned char)line[i];
            int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (i + len > line.size()) break;

            unsigned int cp = 0;
            if (len == 1) cp = c;
            else if (len == 2) cp = c & 0x1F;
            else if (len == 3) cp = c & 0x0F;
            else cp = c & 0x07;
            for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)line[i + k] & 0x3F);

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
            if (w + cw > width) break;
            fitted.append(line, i, len);
            w += cw;
            i += len;
        }

        // 补齐剩余宽度为空格，覆盖上一次的内容
        if (w < width) fitted += std::string((size_t)(width - w), ' ');

        tui->drawText(row, 0, fitted);
    };

    // 标题行
    std::string titleBar = " " + book.title;
    if ((int)titleBar.size() > width - 3) {
        size_t cut = 0;
        int cnt = 0;
        while (cut < titleBar.size() && cnt < width - 6) {
            unsigned char c = (unsigned char)titleBar[cut];
            int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            cut += len;
            cnt++;
        }
        titleBar = titleBar.substr(0, cut) + "...";
    }
    putLine(0, titleBar, 0);

    // 正文
    for (int i = 0; i < linesPerPage; i++) {
        int lineIdx = currentLine + i;
        if (lineIdx >= 0 && lineIdx < (int)book.wrappedContent.size()) {
            const std::string& contentLine = book.wrappedContent[lineIdx];
            bool isChapterTitle = !contentLine.empty() &&
                contentLine.find("第") != std::string::npos &&
                (contentLine.find("章") != std::string::npos ||
                 contentLine.find("节") != std::string::npos ||
                 contentLine.find("回") != std::string::npos);

            if (isChapterTitle) {
                int startCol = (width - (int)contentLine.size()) / 2;
                if (startCol < padding) startCol = padding;
                putLine(1 + i, contentLine, startCol);
            } else {
                putLine(1 + i, contentLine, padding);
            }
        } else {
            putLine(1 + i, "", 0);
        }
    }

    // 状态栏
    int totalPages = (int)((book.wrappedContent.size() + linesPerPage - 1) / linesPerPage);
    int currentPage = (currentLine / linesPerPage) + 1;
    if (totalPages < 1) totalPages = 1;

    std::string statusBar = " 第 " + std::to_string(currentPage) + "/" + std::to_string(totalPages) + " 页 ";
    if (!lastQuery.empty()) {
        statusBar += "| 搜索: " + lastQuery;
        if (!matchLines.empty()) {
            statusBar += " (" + std::to_string(matchIndex + 1) + "/" + std::to_string(matchLines.size()) + ")";
        }
    }
    putLine(height - 1, statusBar, 0);

    tui->refresh();
}

void Reader::handleInput(int key) {
#ifdef PLATFORM_WINDOWS
    if (key == 'q' || key == 'Q' || key == 27) {
        returnToLibrary = true;
        running = false;
    } else if (key == 'j' || key == 1001 || key == 13) {
        scrollDown();
    } else if (key == 'k' || key == 1000) {
        scrollUp();
    } else if (key == 'f' || key == 1003 || key == ' ' || key == 1007) {
        nextPage();
    } else if (key == 'b' || key == 1002 || key == 1006) {
        prevPage();
    } else if (key == 'g' || key == 1004) {
        goToTop();
    } else if (key == 'G' || key == 1005) {
        goToBottom();
    } else if (key == 'h' || key == 'H' || key == '?') {
        showHelp();
    } else if (key == 't' || key == 'T') {
        showTOC();
    } else if (key == '/') {
        openSearchMenu();
    } else if (key == 'F') {
        searchFanqie();
    } else if (key == 'n') {
        if (!matchLines.empty()) {
            matchIndex = (matchIndex + 1) % (int)matchLines.size();
            currentLine = matchLines[matchIndex];
        }
    } else if (key == 'N') {
        if (!matchLines.empty()) {
            matchIndex = (matchIndex - 1 + (int)matchLines.size()) % (int)matchLines.size();
            currentLine = matchLines[matchIndex];
        }
    }
#else
    if (key == 'q' || key == 'Q' || key == 27) {
        returnToLibrary = true;
        running = false;
    } else if (key == 'j' || key == KEY_DOWN || key == 10) {
        scrollDown();
    } else if (key == 'k' || key == KEY_UP) {
        scrollUp();
    } else if (key == 'f' || key == KEY_NPAGE || key == ' ' || key == KEY_RIGHT) {
        nextPage();
    } else if (key == 'b' || key == KEY_PPAGE || key == KEY_LEFT) {
        prevPage();
    } else if (key == 'g' || key == KEY_HOME) {
        goToTop();
    } else if (key == 'G' || key == KEY_END) {
        goToBottom();
    } else if (key == 'h' || key == 'H' || key == '?') {
        showHelp();
    } else if (key == 't' || key == 'T') {
        showTOC();
    } else if (key == '/') {
        openSearchMenu();
    } else if (key == 'F') {
        searchFanqie();
    } else if (key == 'n') {
        if (!matchLines.empty()) {
            matchIndex = (matchIndex + 1) % (int)matchLines.size();
            currentLine = matchLines[matchIndex];
        }
    } else if (key == 'N') {
        if (!matchLines.empty()) {
            matchIndex = (matchIndex - 1 + (int)matchLines.size()) % (int)matchLines.size();
            currentLine = matchLines[matchIndex];
        }
    }
#endif
}

void Reader::scrollUp() {
    if (currentLine > 0) currentLine--;
}

void Reader::scrollDown() {
    if (currentLine + linesPerPage < (int)book.wrappedContent.size()) currentLine++;
}

void Reader::nextPage() {
    currentLine += linesPerPage;
    if (currentLine >= (int)book.wrappedContent.size()) {
        currentLine = (int)book.wrappedContent.size() - linesPerPage;
        if (currentLine < 0) currentLine = 0;
    }
}

void Reader::prevPage() {
    currentLine -= linesPerPage;
    if (currentLine < 0) currentLine = 0;
}

void Reader::goToTop() {
    currentLine = 0;
}

void Reader::goToBottom() {
    if ((int)book.wrappedContent.size() > linesPerPage) {
        currentLine = (int)book.wrappedContent.size() - linesPerPage;
    } else {
        currentLine = 0;
    }
}

void Reader::showTOC() {
    tui->clear();
    int height = tui->getHeight();
    int width = tui->getWidth();

    tui->drawText(0, 0, " 目录");
    for (int i = 0; i < width - 2; i++) tui->drawText(0, 2 + i, " ");

    if (book.chapters.empty()) {
        tui->drawText(2, 2, "暂无目录");
    } else {
        for (size_t i = 0; i < book.chapters.size() && i < (size_t)(height - 5); i++) {
            std::string ch = "  " + std::to_string(i + 1) + ". " + book.chapters[i];
            tui->drawLine((int)(2 + i), 0, ch, width - 4);
        }
    }

    tui->drawText(height - 1, 0, std::string(width, ' '));
    tui->drawText(height - 1, 0, "  按任意键返回");
    tui->refresh();
    tui->getKey();
}

void Reader::showHelp() {
    tui->clear();
    int height = tui->getHeight();
    int width = tui->getWidth();

    tui->drawText(0, 0, " 快捷键");
    for (int i = 0; i < width - 4; i++) tui->drawText(0, 4 + i, " ");

    tui->drawText(2, 2, "翻页：空格 / f / PgDn / →    上页：b / PgUp / ←");
    tui->drawText(3, 2, "滚动：j / ↓              上滚：k / ↑");
    tui->drawText(4, 2, "首尾：g / Home           尾页：G / End");
    tui->drawText(5, 2, "目录：t                  搜索：/");
    tui->drawText(6, 2, "番茄搜索：F              帮助：h / ?");
    tui->drawText(7, 2, "返回书库：q / Esc");
    tui->drawText(9, 2, "搜索菜单中可：文内查找 / 番茄小说搜索（查看/下载）");
    tui->drawText(10, 2, "匹配跳转：n 下一个        N 上一个");

    tui->drawText(height - 1, 0, std::string(width, ' '));
    tui->drawText(height - 1, 0, "  按任意键返回");
    tui->refresh();
    tui->getKey();
}

// ==================== 搜索 ====================

void Reader::openSearchMenu() {
    while (true) {
        tui->clear();
        int height = tui->getHeight();
        int width = tui->getWidth();

        tui->drawText(0, 0, " 搜索");
        for (int i = 0; i < width - 4; i++) tui->drawText(0, 4 + i, " ");

        tui->drawText(2, 2, "1. 在本文中查找");
        tui->drawText(3, 2, "2. 在番茄小说中搜索");
        tui->drawText(4, 2, "3. 在 so-novel 书源中搜索");
        tui->drawText(5, 2, "q. 返回阅读");

        tui->drawText(height - 1, 0, std::string(width, ' '));
        tui->drawText(height - 1, 0, "  请选择 (1/2/3/q)");
        tui->refresh();

        int key = -1;
        while (key == -1) {
            key = tui->getKey();
            if (key == -1) {
#ifdef PLATFORM_WINDOWS
                Sleep(10);
#else
                napms(10);
#endif
            }
        }
        if (key == 'q' || key == 'Q' || key == 27) return;
        if (key == '1') { searchInBook(); return; }
        if (key == '2') { searchFanqie(); return; }
        if (key == '3') { searchSoNovel(); return; }
    }
}

void Reader::searchInBook() {
    int height = tui->getHeight();
    int width = tui->getWidth();
    tui->clear();
    std::string q = tui->promptInput(2, 2, "查找内容: ");
    if (q.empty()) return;

    lastQuery = q;
    matchLines.clear();
    matchIndex = 0;
    for (size_t i = 0; i < book.wrappedContent.size(); i++) {
        if (book.wrappedContent[i].find(q) != std::string::npos) {
            matchLines.push_back((int)i);
        }
    }

    tui->drawText(height - 1, 0, std::string(width, ' '));
    if (matchLines.empty()) {
        tui->drawText(height - 1, 0, "  未找到匹配，按任意键返回");
        tui->refresh();
        tui->getKey();
        lastQuery.clear();
        return;
    }

    currentLine = matchLines[0];
    tui->drawText(height - 1, 0, "  找到 " + std::to_string(matchLines.size()) + " 处，按 n 下一个 / N 上一个");
    tui->refresh();
}

void Reader::searchFanqie() {
#ifndef HAS_CURL
    int height = tui->getHeight();
    tui->clear();
    tui->drawText(2, 2, "番茄搜索需要 libcurl 支持（构建时启用）。");
    tui->drawText(height - 1, 0, "  按任意键返回");
    tui->refresh();
    tui->getKey();
    return;
#else
    int height = tui->getHeight();
    int width = tui->getWidth();
    tui->clear();
    std::string kw = tui->promptInput(2, 2, "搜索番茄小说: ");
    if (kw.empty()) return;

    tui->clear();
    tui->drawText(2, 2, "正在启动番茄服务并搜索，首次可能需等待...");
    tui->refresh();

    FanqieDownloader dl(4);
    std::vector<FanqieDownloader::BookInfo> results = dl.searchBooks(kw);

    if (results.empty()) {
        tui->drawText(4, 2, "未找到相关书籍，按任意键返回");
        tui->refresh();
        tui->getKey();
        return;
    }

    int sel = 0;
    std::vector<bool> marked(results.size(), false);

    auto markedCount = [&]() {
        int n = 0;
        for (bool m : marked) if (m) n++;
        return n;
    };

    while (true) {
        tui->clear();

        int cnt = markedCount();
        std::string hdr = " 番茄搜索: " + kw + "   (结果 " + std::to_string(results.size()) + ")";
        if (cnt > 0) hdr += "   [已选 " + std::to_string(cnt) + " 本]";
        tui->drawText(0, 0, hdr);
        for (int i = (int)hdr.size(); i < width; i++) tui->drawText(0, i, " ");

        int listStart = 2;
        int maxRows = height - 4;
        int start = 0;
        if (sel >= maxRows) start = sel - maxRows + 1;

        for (int i = 0; i < maxRows; i++) {
            int idx = start + i;
            if (idx >= (int)results.size()) break;
            const auto& b = results[idx];

            std::string mark = (idx == sel) ? "> " : "  ";
            mark += marked[idx] ? "[x] " : "[ ] ";

            std::string line = mark + std::to_string(idx + 1) + ". 《" + b.book_name + "》 - " + b.author;
            if (!b.score.empty()) line += " [评分" + b.score + "]";

            tui->drawText(listStart + i, 0, std::string(width, ' '));
            tui->drawLine(listStart + i, 1, line, width - 3);
        }

        if (sel < (int)results.size() && !results[sel].abstract.empty()) {
            tui->drawText(height - 2, 1, std::string(width - 2, ' '));
            tui->drawLine(height - 2, 1, "  " + results[sel].abstract + "...", width - 3);
        }

        std::string status = "  ↑↓ 选择 | 空格/x 标记 | a 全选 | 回车 查看";
        if (cnt > 0) status += " | D 下载已选(" + std::to_string(cnt) + ")";
        status += " | d 下载当前 | q 返回";
        tui->drawText(height - 1, 0, std::string(width, ' '));
        tui->drawLine(height - 1, 0, status, width - 1);
        tui->refresh();

        int key = -1;
        while (key == -1) {
            key = tui->getKey();
            if (key == -1) {
#ifdef PLATFORM_WINDOWS
                Sleep(10);
#else
                napms(10);
#endif
            }
        }
        if (key == 'q' || key == 'Q' || key == 27) return;
#ifdef PLATFORM_WINDOWS
        if (key == 1000) { if (sel > 0) sel--; continue; }
        if (key == 1001) { if (sel + 1 < (int)results.size()) sel++; continue; }
#else
        if (key == KEY_UP) { if (sel > 0) sel--; continue; }
        if (key == KEY_DOWN) { if (sel + 1 < (int)results.size()) sel++; continue; }
#endif

        // 空格 或 x：切换当前项标记
        if (key == ' ' || key == 'x' || key == 'X') {
            marked[sel] = !marked[sel];
            if (sel + 1 < (int)results.size()) sel++;
            continue;
        }

        // a：全选 / 全不选
        if (key == 'a' || key == 'A') {
            bool allMarked = (cnt == (int)results.size());
            for (size_t i = 0; i < marked.size(); i++) marked[i] = !allMarked;
            continue;
        }

        // 回车：查看当前书
        if (key == 13 || key == 10) {
            const auto& b = results[sel];
            tui->clear();
            tui->drawText(2, 2, "正在下载《" + b.book_name + "》，请稍候...");
            tui->drawText(3, 2, "（整本下载完成后进入阅读）");
            tui->refresh();
            std::string text = dl.buildBookText(b.book_id, -1);
            if (text.empty()) {
                tui->drawText(5, 2, "加载失败，按任意键返回");
                tui->refresh();
                tui->getKey();
                continue;
            }
            loadTextAsBook(text, b.book_name);
            bookLoadedInMemory = true;
            return;
        }

        // D：批量下载已标记的书
        if (key == 'D') {
            std::vector<int> targets;
            for (size_t i = 0; i < marked.size(); i++) {
                if (marked[i]) targets.push_back((int)i);
            }
            if (targets.empty()) {
                tui->drawText(height - 1, 0, std::string(width, ' '));
                tui->drawText(height - 1, 0, "  没有选中任何书，先用空格标记");
                tui->refresh();
#ifdef PLATFORM_WINDOWS
                Sleep(700);
#else
                napms(700);
#endif
                continue;
            }

            int okCount = 0;
            int failCount = 0;
            std::vector<std::string> failedNames;
            std::vector<int> okIndices;

            for (size_t t = 0; t < targets.size(); t++) {
                const auto& b = results[targets[t]];

                tui->clear();
                tui->drawText(0, 0, " 批量下载 (" + std::to_string(t + 1) + "/" +
                                    std::to_string(targets.size()) + ")");
                for (int i = 0; i < width; i++) tui->drawText(1, i, " ");

                tui->drawText(2, 2, "《" + b.book_name + "》");
                tui->drawText(3, 2, "作者: " + b.author);
                tui->drawText(5, 2, "已成功: " + std::to_string(okCount));
                tui->drawText(6, 2, "已失败: " + std::to_string(failCount));

                // 简易进度条
                int barW = width - 8;
                if (barW > 0) {
                    int filled = (int)((double)t / (double)targets.size() * (double)barW);
                    std::string bar = "[";
                    bar += std::string((size_t)filled, '#');
                    bar += std::string((size_t)(barW - filled), '.');
                    bar += "]";
                    tui->drawText(8, 2, bar);
                }

                tui->drawText(height - 1, 0, std::string(width, ' '));
                tui->drawText(height - 1, 0, "  下载中，请稍候...");
                tui->refresh();

                if (dl.downloadBook(b.book_id, b.book_name, "", -1)) {
                    okCount++;
                    okIndices.push_back(targets[t]);
                    libraryDirty = true;

                    // 记录来源，供后续自动更新
                    std::string savedPath = fsutil::join(dl.saveDir(),
                                                FanqieDownloader::sanitizeName(b.book_name) + ".txt");
                    cfg.setBookSource(savedPath, b.book_id);
                } else {
                    failCount++;
                    failedNames.push_back(b.book_name);
                }
            }

            // 结果汇总
            tui->clear();
            tui->drawText(0, 0, " 批量下载完成");
            tui->drawText(2, 2, "成功: " + std::to_string(okCount) + " 本");
            if (failCount > 0) {
                tui->drawText(3, 2, "失败: " + std::to_string(failCount) + " 本");
                int row = 5;
                for (size_t i = 0; i < failedNames.size() && row < height - 2; i++, row++) {
                    tui->drawLine(row, 2, "  - " + failedNames[i], width - 4);
                }
            }
            tui->drawText(height - 1, 0, std::string(width, ' '));
            tui->drawText(height - 1, 0, "  按任意键返回列表");
            tui->refresh();
            tui->getKey();

            // 只清除下载成功的标记，失败的保留便于重试
            for (int idx : okIndices) marked[idx] = false;
            continue;
        }

        // d：下载当前这一本
        if (key == 'd') {
            const auto& b = results[sel];
            tui->clear();
            tui->drawText(2, 2, "下载《" + b.book_name + "》中，请稍候...");
            tui->refresh();
            bool ok = dl.downloadBook(b.book_id, b.book_name, "", -1);
            if (ok) {
                libraryDirty = true;
                std::string savedPath = fsutil::join(dl.saveDir(),
                                            FanqieDownloader::sanitizeName(b.book_name) + ".txt");
                cfg.setBookSource(savedPath, b.book_id);
            }
            tui->drawText(5, 2, ok ? "下载完成，按任意键返回" : "下载失败，按任意键返回");
            tui->drawText(6, 2, "保存位置: " + downloadDir());
            tui->refresh();
            tui->getKey();
            if (ok) marked[sel] = false;
        }
    }
#endif
}

void Reader::loadTextAsBook(const std::string& text, const std::string& title) {
    resetBook();
    book.title = title;

    std::string cur;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            book.content.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) book.content.push_back(cur);

    for (size_t i = 0; i < book.content.size(); i++) {
        const std::string& line = book.content[i];
        if (Parser::isChapterTitle(line)) {
            book.chapters.push_back(line);
            book.chapterStarts.push_back((int)i);
        }
    }

    buildWrappedContent();
}

// ==================== 配置 ====================

bool Reader::initConfig() {
    return cfg.load();
}

// ==================== 工具 ====================

std::string Reader::fmtSize(long long bytes) {
    if (bytes < 0) return "";
    if (bytes < 1024) return std::to_string(bytes) + "B";
    double kb = (double)bytes / 1024.0;
    char buf[32];
    if (kb < 1024) {
        std::snprintf(buf, sizeof(buf), "%.0fK", kb);
        return buf;
    }
    double mb = kb / 1024.0;
    if (mb < 1024) {
        std::snprintf(buf, sizeof(buf), "%.1fM", mb);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.2fG", mb / 1024.0);
    return buf;
}

std::string Reader::fmtTime(long long ms) {
    if (ms <= 0) return "";
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tmv{};
#ifdef _WIN32
    if (localtime_s(&tmv, &t) != 0) return "";
#else
    if (!localtime_r(&t, &tmv)) return "";
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

// ==================== 书库 ====================

std::string Reader::downloadDir() {
    std::string base = fsutil::exeDir();
    if (base.empty()) return "";
    return fsutil::join(base, "downloads");
}

void Reader::sortLibrary() {
    // 排序在各分组构建时完成
}

void Reader::rebuildLibrary() {
    libEntries.clear();

    const auto& rec = cfg.recent();

    // 最近打开：按文件是否仍存在分成两组
    std::vector<LibraryEntry> recentValid;
    std::vector<LibraryEntry> recentMissing;

    for (const auto& rf : rec) {
        LibraryEntry e;
        e.path = rf.path;
        e.isRecent = true;
        e.display = fsutil::filename(rf.path);
        e.nameLower = e.display;
        std::transform(e.nameLower.begin(), e.nameLower.end(), e.nameLower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });

        bool ok = fsutil::isFile(rf.path);
        if (ok) {
            e.size = fsutil::fileSize(rf.path);
            e.mtime = fsutil::lastWriteMs(rf.path);
            e.info = fmtSize(e.size);
            std::string ts = fmtTime(e.mtime);
            if (!ts.empty()) e.info += (e.info.empty() ? "" : "  ") + ts;
            recentValid.push_back(e);
        } else {
            e.info = "文件已丢失";
            recentMissing.push_back(e);
        }
    }

    if (!recentValid.empty()) {
        LibraryEntry hdr;
        hdr.isHeader = true;
        hdr.display = "── 最近打开 ──";
        libEntries.push_back(hdr);
        for (auto& e : recentValid) libEntries.push_back(e);
    }

    // 失效记录单独一组，放到最后（下面所有分组之后插入）
    // 先在变量里暂存，末尾再追加

    // 下载目录始终作为书库的一个分组
    std::vector<std::string> allDirs = cfg.dirs();
    std::string dlDir = downloadDir();
    if (!dlDir.empty() && fsutil::isDirectory(dlDir)) {
        bool already = false;
        for (const auto& d : allDirs) {
            if (fsutil::normalize(d) == fsutil::normalize(dlDir)) { already = true; break; }
        }
        if (!already) allDirs.push_back(dlDir);
    }

    for (const auto& dir : allDirs) {
        bool exists = fsutil::isDirectory(dir);

        std::string folderName = fsutil::filename(dir);
        if (folderName.empty()) folderName = dir;

        LibraryEntry hdr;
        hdr.isHeader = true;
        hdr.display = "── " + folderName + (exists ? "" : "（目录不存在）") + " ──";
        libEntries.push_back(hdr);

        if (!exists) continue;

        auto files = Config::scanDir(dir, 4);
        std::vector<LibraryEntry> group;
        for (const auto& f : files) {
            LibraryEntry e;
            e.path = f;

            // 相对路径显示
            std::string rel = f;
            std::string prefix = dir;
            if (!prefix.empty() && !((prefix.back() == '\\') || (prefix.back() == '/'))) {
                prefix += '\\';
            }
            if (rel.size() > prefix.size() &&
                rel.compare(0, prefix.size(), prefix) == 0) {
                rel = rel.substr(prefix.size());
            }
            e.display = rel;

            e.size = fsutil::fileSize(f);
            e.mtime = fsutil::lastWriteMs(f);

            e.nameLower = e.display;
            std::transform(e.nameLower.begin(), e.nameLower.end(), e.nameLower.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });

            e.info = fmtSize(e.size);
            std::string ts = fmtTime(e.mtime);
            if (!ts.empty()) e.info += (e.info.empty() ? "" : "  ") + ts;
            group.push_back(e);
        }

        int mode = cfg.sortMode();
        std::stable_sort(group.begin(), group.end(),
            [mode](const LibraryEntry& a, const LibraryEntry& b) {
                switch (mode) {
                    case 1: return a.size > b.size;
                    case 2: return a.mtime > b.mtime;
                    default: return a.display < b.display;
                }
            });

        for (auto& e : group) libEntries.push_back(e);

        if (group.empty()) {
            LibraryEntry e;
            e.isHeader = true;
            e.display = "   （无电子书文件）";
            libEntries.push_back(e);
        }
    }

    // 失效的最近记录统一放最后
    if (!recentMissing.empty()) {
        LibraryEntry hdr;
        hdr.isHeader = true;
        hdr.display = "── 最近打开（文件已丢失）──";
        libEntries.push_back(hdr);
        for (auto& e : recentMissing) libEntries.push_back(e);
    }

    libraryDirty = false;
}

int Reader::openBookPicker(const std::string& title, const std::vector<LibraryEntry>& allEntries,
                           bool persistCursor) {
    if (allEntries.empty()) return -1;

    int height = tui->getHeight();
    int width = tui->getWidth();

    std::string filter;
    auto modeLabel = [](int m) -> std::string {
        if (m == 1) return "大小";
        if (m == 2) return "时间";
        return "名称";
    };

    std::string wantPath = persistCursor ? cfg.lastCursor() : "";
    int restoreOffset = persistCursor ? cfg.lastCursorOffset() : 0;

    int sel = 0;
    int offset = 0;
    bool firstDraw = true;

    while (true) {
        std::vector<int> selectable;
        for (size_t i = 0; i < allEntries.size(); i++) {
            if (allEntries[i].isHeader) continue;
            if (!filter.empty() && allEntries[i].nameLower.find(filter) == std::string::npos) continue;
            selectable.push_back((int)i);
        }

        int visible = height - 3;
        if (visible < 3) visible = 3;

        if (selectable.empty()) {
            tui->clear();
            tui->drawText(0, 0, " " + title);
            for (int i = (int)title.size(); i < width; i++) tui->drawText(0, i, " ");

            // 即使没有书，也把分组标题画出来，让用户知道有哪些目录
            int row = 2;
            for (size_t i = 0; i < allEntries.size() && row < height - 2; i++) {
                if (!allEntries[i].isHeader) continue;
                tui->drawLine(row, 1, allEntries[i].display, width - 3);
                row++;
            }

            if (row < height - 2) {
                row++;
                std::string hint = filter.empty()
                    ? "（还没有书籍。按 a 添加书库文件夹，或按 F 搜索番茄小说下载）"
                    : ("没有匹配「" + filter + "」的条目");
                tui->drawLine(row, 2, hint, width - 4);
            }

            tui->drawText(height - 1, 0, std::string(width, ' '));
            tui->drawText(height - 1, 0, "  Esc 清除过滤 | a 添加文件夹 | F 番茄搜索 | r 刷新 | q 返回");
            tui->refresh();

            int key = -1;
            while (key == -1) {
                key = tui->getKey();
                if (key == -1) {
#ifdef PLATFORM_WINDOWS
                    Sleep(15);
#else
                    napms(15);
#endif
                }
            }
            if (key == 'q' || key == 'Q') {
                if (!filter.empty()) { filter.clear(); sel = 0; offset = 0; continue; }
                return -1;
            }
            if (key == 27) {
                if (!filter.empty()) { filter.clear(); continue; }
                return -1;
            }
            if (key == 'a' || key == 'A') return -3;
            if (key == 'r' || key == 'R') return -2;
            if (key == 'F') return -6;
#ifdef HAS_MUSIC
            if (key == 'M') return -7;
#endif
            if (key == 'U') return -8;
            if (key == 8 || key == 127) {
                if (!filter.empty()) {
                    while (!filter.empty() && ((unsigned char)filter.back() & 0xC0) == 0x80) filter.pop_back();
                    if (!filter.empty()) filter.pop_back();
                }
            }
            continue;
        }

        if (sel >= (int)selectable.size()) sel = (int)selectable.size() - 1;
        if (sel < 0) sel = 0;

        if (firstDraw && persistCursor && !wantPath.empty()) {
            for (size_t i = 0; i < selectable.size(); i++) {
                if (allEntries[selectable[i]].path == wantPath) {
                    sel = (int)i;
                    offset = restoreOffset;
                    break;
                }
            }
        }
        firstDraw = false;

        int flatIdx = selectable[sel];

        if (flatIdx < offset) offset = flatIdx;
        if (flatIdx >= offset + visible) offset = flatIdx - visible + 1;
        if (offset < 0) offset = 0;

        tui->clear();

        std::string header = " " + title;
        if (!filter.empty()) header += "  [过滤: " + filter + "]";
        header += "  [排序: " + modeLabel(cfg.sortMode()) + "]";
        tui->drawText(0, 0, header);
        for (int i = (int)header.size(); i < width; i++) tui->drawText(0, i, " ");

        int row = 1;
        for (int i = offset; i < (int)allEntries.size() && row <= visible; i++) {
            const auto& e = allEntries[i];

            if (!filter.empty() && !e.isHeader &&
                e.nameLower.find(filter) == std::string::npos) {
                continue;
            }

            tui->drawText(row, 0, std::string(width, ' '));

            if (e.isHeader) {
                tui->drawLine(row, 1, e.display, width - 3);
            } else {
                bool isSel = (i == flatIdx);
                std::string mark = isSel ? "> " : "  ";
                std::string info = e.info;

                int infoW = (int)info.size();
                int nameW = width - 2 - infoW - 2;
                if (nameW < 12) { info.clear(); infoW = 0; nameW = width - 2; }

                std::string name = e.display;
                if ((int)name.size() > nameW) {
                    size_t cut = 0;
                    int cnt = 0;
                    while (cut < name.size() && cnt < nameW - 3) {
                        unsigned char c = (unsigned char)name[cut];
                        int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
                        cut += len;
                        cnt++;
                    }
                    name = name.substr(0, cut) + "...";
                }

                tui->drawText(row, 0, mark + name);
                if (!info.empty()) tui->drawText(row, width - infoW - 1, info);
            }
            row++;
        }

        tui->drawText(height - 1, 0, std::string(width, ' '));
        std::string status = " " + std::to_string(sel + 1) + "/" + std::to_string(selectable.size());
        status += "  ↑↓ 选择 | Enter 打开 | / 过滤 | s 排序 | F 番茄 | ";
#ifdef HAS_MUSIC
        status += "M 音乐 | ";
#endif
        status += "a 添加 | d 移除 | r 刷新 | U 设置 | q 退出";
        tui->drawLine(height - 1, 0, status, width - 1);
        tui->refresh();

        int key = -1;
        while (key == -1) {
            key = tui->getKey();
            if (key == -1) {
#ifdef PLATFORM_WINDOWS
                Sleep(15);
#else
                napms(15);
#endif
            }
        }

        // 有过滤时 q 先清除过滤，避免误退出；无过滤时才真正退出
        if (key == 'q' || key == 'Q') {
            if (!filter.empty()) {
                filter.clear();
                sel = 0;
                offset = 0;
                continue;
            }
            if (persistCursor) cfg.setLastCursor(allEntries[flatIdx].path, offset);
            return -1;
        }
        if (key == 27) {
            if (!filter.empty()) { filter.clear(); sel = 0; offset = 0; continue; }
            if (persistCursor) cfg.setLastCursor(allEntries[flatIdx].path, offset);
            return -1;
        }
        if (key == 'r' || key == 'R') return -2;
        if (key == 'a' || key == 'A') return -3;
        if (key == 'd' || key == 'D') return -4;
        if (key == 's' || key == 'S') {
            cfg.setSortMode((cfg.sortMode() + 1) % 3);
            return -5;
        }
        if (key == '/') {
            std::string q = tui->promptInput(height - 1, 0, "过滤: ", filter);
            filter = q;
            std::transform(filter.begin(), filter.end(), filter.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            sel = 0;
            offset = 0;
            continue;
        }
        if (key == 'F') return -6;
#ifdef HAS_MUSIC
        if (key == 'M') return -7;
#endif
        if (key == 'U') return -8;
        if (key == 8 || key == 127) {
            if (!filter.empty()) {
                while (!filter.empty() && ((unsigned char)filter.back() & 0xC0) == 0x80) filter.pop_back();
                if (!filter.empty()) filter.pop_back();
                sel = 0;
                offset = 0;
            }
            continue;
        }

#ifdef PLATFORM_WINDOWS
        if (key == 1000) { if (sel > 0) sel--; continue; }
        if (key == 1001) { if (sel + 1 < (int)selectable.size()) sel++; continue; }
        if (key == 1002) { sel -= visible; if (sel < 0) sel = 0; continue; }
        if (key == 1003) { sel += visible; if (sel >= (int)selectable.size()) sel = (int)selectable.size() - 1; continue; }
        if (key == 1004) { sel = 0; continue; }
        if (key == 1005) { sel = (int)selectable.size() - 1; continue; }
#else
        if (key == KEY_UP) { if (sel > 0) sel--; continue; }
        if (key == KEY_DOWN) { if (sel + 1 < (int)selectable.size()) sel++; continue; }
        if (key == KEY_PPAGE) { sel -= visible; if (sel < 0) sel = 0; continue; }
        if (key == KEY_NPAGE) { sel += visible; if (sel >= (int)selectable.size()) sel = (int)selectable.size() - 1; continue; }
        if (key == KEY_HOME) { sel = 0; continue; }
        if (key == KEY_END) { sel = (int)selectable.size() - 1; continue; }
        if (key == KEY_RESIZE) { height = tui->getHeight(); continue; }
#endif
        if (key == 13 || key == 10) {
            if (persistCursor) cfg.setLastCursor(allEntries[flatIdx].path, offset);
            return flatIdx;
        }
    }
}

void Reader::addLibraryFolderDialog() {
    tui->clear();
    tui->drawText(2, 2, "输入要添加的文件夹路径（留空取消）：");
    std::string p = tui->promptInput(4, 2, "路径: ");
    if (p.empty()) return;

    if (!fsutil::isDirectory(p)) {
        tui->drawText(6, 2, "目录不存在或无法访问，按任意键返回");
        tui->refresh();
        tui->getKey();
        return;
    }

    cfg.addLibraryDir(p);
    libraryDirty = true;

    auto files = Config::scanDir(p, 4);
    tui->drawText(6, 2, "已添加，扫描到 " + std::to_string(files.size()) + " 本电子书");
    tui->drawText(7, 2, "按任意键返回");
    tui->refresh();
    tui->getKey();
}

void Reader::removeLibraryFolderDialog() {
    const auto& dirs = cfg.dirs();

    if (dirs.empty()) {
        tui->clear();
        tui->drawText(4, 2, "还没有添加任何文件夹，按任意键返回");
        tui->refresh();
        tui->getKey();
        return;
    }

    std::vector<LibraryEntry> items;
    for (const auto& d : dirs) {
        LibraryEntry e;
        e.path = d;
        e.display = d;
        e.nameLower = d;
        std::transform(e.nameLower.begin(), e.nameLower.end(), e.nameLower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        items.push_back(e);
    }

    int idx = openBookPicker("移除书库文件夹", items, false);
    if (idx < 0) return;

    cfg.removeLibraryDir(items[idx].path);
    libraryDirty = true;
}

std::string Reader::showLibrary() {
    if (cfg.dirs().empty() && cfg.recent().empty()) {
        tui->clear();
        tui->drawText(2, 2, "书库为空。");
        tui->drawText(4, 2, "你可以添加文件夹，程序会递归扫描其中的 txt/epub/docx/pdf 文件。");
        tui->drawText(6, 2, "现在添加一个文件夹？(y/n)");
        tui->refresh();

        int k = -1;
        while (k == -1) {
            k = tui->getKey();
            if (k == -1) {
#ifdef PLATFORM_WINDOWS
                Sleep(15);
#else
                napms(15);
#endif
            }
        }
        if (k == 'y' || k == 'Y') addLibraryFolderDialog();
    }

    while (true) {
        if (libraryDirty) rebuildLibrary();

        int rc = openBookPicker("书库", libEntries, true);

        if (rc == -1) return "";
        if (rc == -2) { libraryDirty = true; continue; }
        if (rc == -3) { addLibraryFolderDialog(); continue; }
        if (rc == -4) { removeLibraryFolderDialog(); continue; }
        if (rc == -5) { libraryDirty = true; continue; }
        if (rc == -6) {
            searchFanqie();
            if (bookLoadedInMemory) return "";  // 内容已在内存，由 main 直接阅读
            libraryDirty = true;
            continue;
        }
#ifdef HAS_MUSIC
        if (rc == -7) {
            requestMusic = true;
            return "";
        }
#endif
        if (rc == -8) {
            showSettings();
            libraryDirty = true;
            continue;
        }
        if (rc >= 0 && rc < (int)libEntries.size()) {
            libraryDirty = true;   // 下次进书库重新扫描，反映新增文件
            return libEntries[rc].path;
        }
    }
}

// ==================== 番茄回退 ====================

std::string Reader::extractBookTitle(const std::string& filePath) {
    // 取文件名去扩展名
    std::string name = fsutil::stem(filePath);

    // 去掉常见的站点/来源标记： [www.xxx.com]、【xxx】、(xxx)
    // 注意：分隔符可能是多字节 UTF-8，必须按完整长度比较
    auto stripBracketed = [](std::string s, const std::string& open, const std::string& close,
                             bool onlySite) {
        std::string out;
        size_t i = 0;
        while (i < s.size()) {
            if (s.size() - i >= open.size() && s.compare(i, open.size(), open) == 0) {
                size_t end = s.find(close, i + open.size());
                if (end != std::string::npos) {
                    std::string inner = s.substr(i + open.size(), end - i - open.size());
                    bool isSite = inner.find("www.") != std::string::npos ||
                                  inner.find(".com") != std::string::npos ||
                                  inner.find(".net") != std::string::npos ||
                                  inner.find(".org") != std::string::npos ||
                                  inner.find("ibiqiuge") != std::string::npos;
                    if (!onlySite || isSite) {
                        i = end + close.size();
                        continue;
                    }
                }
            }
            out += s[i];
            i++;
        }
        return out;
    };

    // 站点标记：只去掉含域名的
    name = stripBracketed(name, "[", "]", true);
    // 附加信息括号：整段去掉（「完结」「精校版」等）
    name = stripBracketed(name, "【", "】", false);
    name = stripBracketed(name, "(", ")", false);
    name = stripBracketed(name, "（", "）", false);
    // 书名号：只去符号，保留内部书名
    {
        std::string tmp;
        tmp.reserve(name.size());
        size_t i = 0;
        while (i < name.size()) {
            if (name.size() - i >= 3 && name.compare(i, 3, "《") == 0) { i += 3; continue; }
            if (name.size() - i >= 3 && name.compare(i, 3, "》") == 0) { i += 3; continue; }
            tmp += name[i];
            i++;
        }
        name = tmp;
    }

    // 去掉末尾的 "(1)"、" 2" 之类的重复副本标记
    while (!name.empty() && (name.back() == ' ' || name.back() == '-' || name.back() == '_')) {
        name.pop_back();
    }
    {
        size_t sp = name.find_last_of(" \t");
        if (sp != std::string::npos) {
            std::string tail = name.substr(sp + 1);
            bool digitsOnly = !tail.empty();
            for (char c : tail) if (!std::isdigit((unsigned char)c)) { digitsOnly = false; break; }
            if (digitsOnly) name = name.substr(0, sp);
        }
    }

    return trimStrLocal(name);
}

std::string Reader::normalizeTitle(const std::string& title) {
    std::string out;
    out.reserve(title.size());

    size_t i = 0;
    while (i < title.size()) {
        unsigned char c = (unsigned char)title[i];
        int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + len > title.size()) len = 1;

        unsigned int cp = 0;
        if (len == 1) cp = c;
        else if (len == 2) cp = c & 0x1F;
        else if (len == 3) cp = c & 0x0F;
        else cp = c & 0x07;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)title[i + k] & 0x3F);

        bool skip = false;
        if (cp < 0x80) {
            // ASCII：去掉空白与常见标点
            if (std::isspace((int)cp)) skip = true;
            else if (cp == ':' || cp == '-' || cp == '_' || cp == '.' ||
                     cp == ',' || cp == '!' || cp == '?' || cp == '\'' || cp == '"') skip = true;
            else if (std::isupper((int)cp)) cp = (unsigned int)std::tolower((int)cp);
        } else {
            // 全角标点
            if (cp == 0x3000 || cp == 0xFF1A || cp == 0xFF0C || cp == 0x3002 ||
                cp == 0xFF01 || cp == 0xFF1F || cp == 0x3001 || cp == 0xFF08 ||
                cp == 0xFF09 || cp == 0x3010 || cp == 0x3011 || cp == 0x2018 ||
                cp == 0x2019 || cp == 0x201C || cp == 0x201D || cp == 0x300A ||
                cp == 0x300B || cp == 0xFF0D || cp == 0x2014) skip = true;
        }

        if (!skip) {
            if (len == 1) out += (char)cp;
            else out.append(title, i, (size_t)len);
        }
        i += (size_t)len;
    }
    return out;
}

#ifdef HAS_CURL
std::string Reader::tryFanqieFallback(const std::string& filePath) {
    std::string wanted = extractBookTitle(filePath);
    if (wanted.empty()) return "";

    std::string wantedNorm = normalizeTitle(wanted);

    FanqieDownloader dl(4);
    if (!dl.ensureService()) return "";

    auto results = dl.searchBooks(wanted);
    if (results.empty()) return "";

    // 只接受书名归一化后完全一致的
    const FanqieDownloader::BookInfo* hit = nullptr;
    for (const auto& b : results) {
        if (normalizeTitle(b.book_name) == wantedNorm) { hit = &b; break; }
    }
    if (!hit) return "";

    std::string outPath = fsutil::join(dl.saveDir(),
                                       FanqieDownloader::sanitizeName(hit->book_name) + ".txt");
    if (!dl.downloadBook(hit->book_id, hit->book_name, "", -1)) return "";
    if (!fsutil::isFile(outPath)) return "";
    cfg.setBookSource(outPath, hit->book_id);
    return outPath;
}
#endif

bool Reader::loadBookOrRecover(const std::string& path, std::string& failReason) {
    if (loadBook(path)) return true;

    // 只有文件真的不存在时才去番茄找；解析失败（文件损坏等）不联网
    if (!fsutil::exists(path)) {
        if (!autoFallback) {
            failReason = "文件不存在";
            return false;
        }

#ifdef HAS_CURL
        std::string got = tryFanqieFallback(path);
        if (!got.empty() && loadBook(got)) return true;
        failReason = "文件不存在，番茄未找到同名书籍";
#else
        failReason = "文件不存在（未启用网络功能）";
#endif
        return false;
    }

    failReason = "文件存在但无法解析";
    return false;
}

// ==================== 设置面板 ====================

void Reader::showSettings() {
    int sel = 0;
    const int ITEM_COUNT = 5;

    while (true) {
        int h = tui->getHeight();
        int w = tui->getWidth();

        tui->clear();
        tui->drawText(0, 0, std::string(w, ' '));
        tui->drawLine(0, 0, " 设置", w - 1);

        auto drawItem = [&](int row, int idx, const char* label, bool value) {
            bool isSel = (idx == sel);
            std::string mark = isSel ? "> " : "  ";
            std::string line = mark + label;
            // 右侧对齐 [开] / [关]
            std::string state = value ? "[开]" : "[关]";
            int pad = w - (int)line.size() - (int)state.size() - 2;
            if (pad > 0) line += std::string((size_t)pad, ' ');
            line += state;

            tui->drawText(row, 0, std::string(w, ' '));
            tui->drawLine(row, 1, line, w - 2);
        };

        tui->drawText(2, 2, "自动更新");
        drawItem(3, 0, "  打开番茄书时检查更新", cfg.autoUpdateBooks());
        drawItem(4, 1, "  启动时检查番茄下载器更新", cfg.autoUpdateDownloader());

        tui->drawText(6, 2, "so-novel");
        drawItem(7, 2, "  自动启动 so-novel 服务", cfg.autoStartSoNovel());
        {
            std::string home = cfg.soNovelHome();
            if (home.empty()) {
                std::string found = SoNovel::findHome();
                home = found.empty() ? "(未找到)" : found;
            }
            bool isSel = (sel == 3);
            std::string mark = isSel ? "> " : "  ";
            std::string line = mark + "  安装目录: " + home;
            tui->drawText(8, 0, std::string(w, ' '));
            tui->drawLine(8, 1, line, w - 2);
        }
        drawItem(9, 4, "  so-novel 自动更新（内置）",
                 true);   // 由 so-novel 自己的 config.ini 控制，这里只展示

        tui->drawText(12, 2, "说明：");
        tui->drawText(13, 4, "· 番茄书更新：打开时增量拉取新章节");
        tui->drawText(14, 4, "· so-novel 服务：未运行时自动拉起 sonovel.exe");
        tui->drawText(15, 4, "· 按 Enter 可编辑 so-novel 安装目录");
        tui->drawText(16, 4, "· 设置保存在 ebook_reader.config.json");

        tui->drawText(h - 1, 0, std::string(w, ' '));
        tui->drawText(h - 1, 0, "  ↑↓ 选择 | 空格/回车 切换 | q/Esc 返回");

        tui->refresh();

        int key = -1;
        while (key == -1) {
            key = tui->getKey();
            if (key == -1) {
#ifdef PLATFORM_WINDOWS
                Sleep(15);
#else
                napms(15);
#endif
            }
        }

        if (key == 'q' || key == 'Q' || key == 27) return;

#ifdef PLATFORM_WINDOWS
        if (key == 1000) { if (sel > 0) sel--; continue; }
        if (key == 1001) { if (sel + 1 < ITEM_COUNT) sel++; continue; }
#else
        if (key == KEY_UP) { if (sel > 0) sel--; continue; }
        if (key == KEY_DOWN) { if (sel + 1 < ITEM_COUNT) sel++; continue; }
#endif

        if (key == ' ' || key == 13 || key == 10) {
            if (sel == 0) {
                cfg.setAutoUpdateBooks(!cfg.autoUpdateBooks());
            } else if (sel == 1) {
                cfg.setAutoUpdateDownloader(!cfg.autoUpdateDownloader());
            } else if (sel == 2) {
                cfg.setAutoStartSoNovel(!cfg.autoStartSoNovel());
            } else if (sel == 3) {
                // 编辑 so-novel 安装目录
                std::string cur = cfg.soNovelHome();
                std::string input = tui->promptInput(h - 1, 0, "安装目录: ", cur);
                if (!input.empty()) {
                    if (fsutil::isDirectory(input)) {
                        cfg.setSoNovelHome(input);
                    } else {
                        // 目录无效时提示，但不保存
                        tui->drawText(h - 2, 2, "目录不存在，未保存");
                        tui->refresh();
#ifdef PLATFORM_WINDOWS
                        Sleep(1200);
#else
                        napms(1200);
#endif
                    }
                }
            }
            continue;
        }
    }
}

// ==================== so-novel 搜索 ====================

void Reader::searchSoNovel() {
#ifndef HAS_CURL
    int height = tui->getHeight();
    tui->clear();
    tui->drawText(2, 2, "so-novel 搜索需要 libcurl 支持。");
    tui->drawText(height - 1, 0, "  按任意键返回");
    tui->refresh();
    tui->getKey();
    return;
#else
    int height = tui->getHeight();
    int width = tui->getWidth();

    SoNovel svc;

    // 服务未运行时尝试自动拉起（可在设置里关闭）
    if (!svc.isAvailable()) {
        if (cfg.autoStartSoNovel()) {
            tui->clear();
            tui->drawText(2, 2, "正在启动 so-novel 服务...");
            tui->drawText(4, 2, "首次启动可能需要 10~20 秒");
            tui->refresh();

            if (!svc.ensureService(cfg.soNovelHome(), 25)) {
                tui->clear();
                tui->drawText(2, 2, "无法启动 so-novel：" + svc.lastError());
                tui->drawText(4, 2, "请确认 sonovel.exe 的位置，或手动启动后重试。");
                tui->drawText(6, 2, "默认查找路径：");
                tui->drawText(7, 4, "· 程序同目录 / tools/sonovel");
                tui->drawText(8, 4, "· D:\\sonovel-windows");
                tui->drawText(9, 4, "· 环境变量 SONOVEL_HOME");
                tui->drawText(11, 2, "也可在设置里关闭自动启动。");
                tui->drawText(height - 1, 0, "  按任意键返回");
                tui->refresh();
                tui->getKey();
                return;
            }
        } else {
            tui->clear();
            tui->drawText(2, 2, "so-novel 服务未运行（自动启动已在设置中关闭）。");
            tui->drawText(4, 2, "请手动启动 sonovel.exe 后重试。");
            tui->drawText(height - 1, 0, "  按任意键返回");
            tui->refresh();
            tui->getKey();
            return;
        }
    }

    tui->clear();
    tui->drawText(2, 2, "so-novel 书源搜索（聚合所有书源，可能较慢）");
    std::string kw = tui->promptInput(4, 2, "关键词: ");
    if (kw.empty()) return;

    tui->clear();
    tui->drawText(2, 2, "正在聚合搜索「" + kw + "」...");
    tui->drawText(4, 2, "so-novel 会同时查询所有书源，通常需要 10~30 秒");
    tui->refresh();

    std::vector<SoNovel::Book> results = svc.search(kw);

    if (results.empty()) {
        tui->drawText(6, 2, "未找到结果：" + svc.lastError());
        tui->drawText(height - 1, 0, "  按任意键返回");
        tui->refresh();
        tui->getKey();
        return;
    }

    std::string filter;
    std::vector<int> filtered;
    auto refreshFilter = [&]() {
        filtered.clear();
        std::string f = filter;
        std::transform(f.begin(), f.end(), f.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (size_t i = 0; i < results.size(); i++) {
            if (f.empty()) { filtered.push_back((int)i); continue; }
            std::string hay = results[i].bookName + " " + results[i].author +
                              " " + results[i].sourceName;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (hay.find(f) != std::string::npos) filtered.push_back((int)i);
        }
    };
    refreshFilter();

    int sel = 0;
    int offset = 0;

    while (true) {
        int h = tui->getHeight();
        int w = tui->getWidth();

        std::string hdr = " so-novel: " + kw + "  (" + std::to_string(filtered.size()) +
                          "/" + std::to_string(results.size()) + " 条)";
        if (!filter.empty()) hdr += "  过滤: " + filter;

        tui->clear();
        tui->drawLine(0, 0, hdr, w - 1);
        for (int i = (int)hdr.size(); i < w; i++) tui->drawText(0, i, " ");

        int listH = h - 2;
        if (listH < 3) listH = 3;

        if (sel < 0) sel = 0;
        if (sel >= (int)filtered.size()) sel = (int)filtered.size() - 1;
        if (sel < 0) sel = 0;

        if (sel < offset) offset = sel;
        if (sel >= offset + listH) offset = sel - listH + 1;
        if (offset < 0) offset = 0;

        for (int i = 0; i < listH; i++) {
            int row = 1 + i;
            int idx = offset + i;
            tui->drawText(row, 0, std::string(w, ' '));
            if (idx >= (int)filtered.size()) continue;

            const SoNovel::Book& b = results[filtered[idx]];
            bool isSel = (idx == sel);

            std::string mark = isSel ? "> " : "  ";
            std::string line = mark + b.bookName + " - " + b.author + "  [" + b.sourceName + "]";

            int avail = w - 2;
            std::string fitted;
            int wsum = 0;
            size_t pos = 0;
            while (pos < line.size()) {
                unsigned char c = (unsigned char)line[pos];
                int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2
                        : (c & 0xF0) == 0xE0 ? 3 : 4;
                if (pos + len > line.size()) break;
                unsigned int cp = 0;
                if (len == 1) cp = c;
                else if (len == 2) cp = c & 0x1F;
                else if (len == 3) cp = c & 0x0F;
                else cp = c & 0x07;
                for (int k = 1; k < len; k++)
                    cp = (cp << 6) | ((unsigned char)line[pos + k] & 0x3F);
                bool wide = (cp >= 0x1100 && (
                    cp <= 0x115F || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                    (cp >= 0xFF00 && cp <= 0xFF60)));
                int cw = wide ? 2 : 1;
                if (wsum + cw > avail) break;
                fitted.append(line, pos, (size_t)len);
                wsum += cw;
                pos += (size_t)len;
            }
            tui->drawLine(row, 1, fitted, avail);
        }

        tui->drawText(h - 1, 0, std::string(w, ' '));
        tui->drawText(h - 1, 0,
            "  ↑↓ 选择 | Enter 下载到本地 | / 过滤 | q 返回");

        tui->refresh();

        int key = -1;
        while (key == -1) {
            key = tui->getKey();
            if (key == -1) {
#ifdef PLATFORM_WINDOWS
                Sleep(15);
#else
                napms(15);
#endif
            }
        }

        if (key == 'q' || key == 'Q') return;

        if (key == '/') {
            filter = tui->promptInput(h - 1, 0, "过滤: ", filter);
            refreshFilter();
            sel = 0;
            offset = 0;
            continue;
        }

#ifdef PLATFORM_WINDOWS
        if (key == 1000) { if (sel > 0) sel--; continue; }
        if (key == 1001) { if (sel + 1 < (int)filtered.size()) sel++; continue; }
        if (key == 1002) { sel -= listH; if (sel < 0) sel = 0; continue; }
        if (key == 1003) { sel += listH; if (sel >= (int)filtered.size()) sel = (int)filtered.size() - 1; continue; }
#else
        if (key == KEY_UP) { if (sel > 0) sel--; continue; }
        if (key == KEY_DOWN) { if (sel + 1 < (int)filtered.size()) sel++; continue; }
        if (key == KEY_PPAGE) { sel -= listH; if (sel < 0) sel = 0; continue; }
        if (key == KEY_NPAGE) { sel += listH; if (sel >= (int)filtered.size()) sel = (int)filtered.size() - 1; continue; }
#endif

        if (key == 13 || key == 10) {
            if (filtered.empty()) continue;
            const SoNovel::Book& b = results[filtered[sel]];

            tui->clear();
            tui->drawText(2, 2, "正在下载《" + b.bookName + "》...");
            tui->drawText(3, 2, "书源: " + b.sourceName);
            tui->drawText(5, 2, "so-novel 会逐章抓取，章节多的书可能要几分钟");
            tui->drawText(6, 2, "请耐心等待，不要关闭窗口");
            tui->refresh();

            std::string fileName = svc.download(b, "txt");
            if (fileName.empty()) {
                tui->drawText(8, 2, "下载失败: " + svc.lastError());
                tui->drawText(h - 1, 0, "  按任意键返回列表");
                tui->refresh();
                tui->getKey();
                continue;
            }

            tui->drawText(8, 2, "服务端完成: " + fileName);
            tui->drawText(9, 2, "正在复制到本地...");
            tui->refresh();

            std::string localDir = downloadDir();
            std::string localPath;
            if (!svc.fetchToLocal(fileName, localDir, localPath)) {
                tui->drawText(11, 2, "复制失败: " + svc.lastError());
                tui->drawText(h - 1, 0, "  按任意键返回列表");
                tui->refresh();
                tui->getKey();
                continue;
            }

            tui->drawText(11, 2, "已保存: " + localPath);
            tui->drawText(13, 2, "按任意键开始阅读");
            tui->refresh();
            tui->getKey();

            if (Parser::parse(localPath, book)) {
                buildWrappedContent();
                cfg.recordFile(localPath);
                libraryDirty = true;
                bookLoadedInMemory = true;
                return;
            }

            tui->drawText(13, 2, "文件已保存，但解析失败，按任意键返回书库");
            libraryDirty = true;
            tui->refresh();
            tui->getKey();
            return;
        }
    }
#endif
}
