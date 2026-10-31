#include "parser.h"
#include "fs_util.h"
#include "encoding.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

std::string Parser::getExtension(const std::string& filename) {
    size_t pos = filename.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string ext = filename.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

bool Parser::parse(const std::string& filename, Book& book) {
    std::string ext = getExtension(filename);
    if (ext == "txt") return parseTXT(filename, book);
    if (ext == "epub") return parseEPUB(filename, book);
    if (ext == "docx") return parseDOCX(filename, book);
    if (ext == "pdf") return parsePDF(filename, book);
    return parseTXT(filename, book);
}

bool Parser::parseTXT(const std::string& filename, Book& book) {
    std::string raw;
    if (!fsutil::readAll(filename, raw)) return false;

    std::string utf8 = encoding::toUtf8(raw);

    book.title = fsutil::stem(filename);
    book.author = "";

    // 按行切分（兼容 \r\n 与 \n）
    std::string line;
    int lineIndex = 0;
    auto flushLine = [&]() {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        book.content.push_back(line);

        if (!line.empty()) {
            if ((line.find("第") != std::string::npos &&
                 (line.find("章") != std::string::npos || line.find("节") != std::string::npos ||
                  line.find("回") != std::string::npos)) ||
                (line.find("Chapter") != std::string::npos && line.find(" ") != std::string::npos)) {
                std::string chapterTitle = line;
                // 按 UTF-8 字符数截断，避免切坏多字节字符
                if (chapterTitle.size() > 180) {
                    size_t cut = 0;
                    int cnt = 0;
                    while (cut < chapterTitle.size() && cnt < 57) {
                        unsigned char c = (unsigned char)chapterTitle[cut];
                        int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
                        cut += len;
                        cnt++;
                    }
                    chapterTitle = chapterTitle.substr(0, cut) + "...";
                }
                book.chapters.push_back(chapterTitle);
                book.chapterStarts.push_back(lineIndex);
            }
        }
        lineIndex++;
        line.clear();
    };

    for (char c : utf8) {
        if (c == '\n') flushLine();
        else line += c;
    }
    if (!line.empty()) flushLine();

    return true;
}
