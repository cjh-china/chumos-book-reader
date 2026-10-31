#include "markup_util.h"
#include <cctype>
#include <cstdlib>

namespace markup {

namespace {

bool startsWithAt(const std::string& s, size_t p, const char* lit) {
    size_t i = 0;
    while (lit[i]) {
        if (p + i >= s.size()) return false;
        char a = s[p + i];
        char b = lit[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
        i++;
    }
    return true;
}

bool isSpaceByte(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// 判断标签名是否为块级（产生换行）
bool isBlockTag(const std::string& name) {
    static const char* blocks[] = {
        "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6",
        "li", "tr", "td", "th", "section", "article", "header", "footer",
        "blockquote", "pre", "hr", "table", "ul", "ol", "dl", "dt", "dd"
    };
    for (const char* b : blocks) {
        if (name == b) return true;
    }
    return false;
}

} // namespace

std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        bool handled = true;

        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent == "nbsp") out += ' ';
        else if (ent == "mdash") out += "\xE2\x80\x94";
        else if (ent == "ndash") out += "\xE2\x80\x93";
        else if (ent == "hellip") out += "\xE2\x80\xA6";
        else if (ent == "ldquo") out += "\xE2\x80\x9C";
        else if (ent == "rdquo") out += "\xE2\x80\x9D";
        else if (ent == "lsquo") out += "\xE2\x80\x98";
        else if (ent == "rsquo") out += "\xE2\x80\x99";
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            bool ok = true;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                if (ent.size() == 2) ok = false;
                for (size_t k = 2; k < ent.size() && ok; k++) {
                    char c = ent[k];
                    int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else { ok = false; break; }
                    cp = cp * 16 + (unsigned long)d;
                }
            } else {
                if (ent.size() == 1) ok = false;
                for (size_t k = 1; k < ent.size() && ok; k++) {
                    char c = ent[k];
                    if (c < '0' || c > '9') { ok = false; break; }
                    cp = cp * 10 + (unsigned long)(c - '0');
                }
            }

            if (ok && cp > 0 && cp <= 0x10FFFF) {
                // 编码为 UTF-8
                if (cp < 0x80) {
                    out += (char)cp;
                } else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
            } else {
                handled = false;
            }
        } else {
            handled = false;
        }

        if (handled) i = semi;
        else out += s[i];
    }

    return out;
}

std::string htmlToText(const std::string& html) {
    std::string out;
    out.reserve(html.size());

    bool pendingSpace = false;
    bool pendingBreak = false;
    bool inScript = false;
    bool inStyle = false;
    bool inHead = false;

    auto flushSpace = [&]() {
        if (pendingBreak) {
            if (!out.empty()) out += '\n';
            pendingBreak = false;
            pendingSpace = false;
        } else if (pendingSpace) {
            if (!out.empty() && out.back() != '\n') out += ' ';
            pendingSpace = false;
        }
    };

    for (size_t i = 0; i < html.size();) {
        if (html[i] == '<') {
            // 注释
            if (startsWithAt(html, i, "<!--")) {
                size_t e = html.find("-->", i + 4);
                i = (e == std::string::npos) ? html.size() : e + 3;
                continue;
            }

            size_t end = html.find('>', i);
            if (end == std::string::npos) break;

            std::string inner = html.substr(i + 1, end - i - 1);
            bool closing = !inner.empty() && inner[0] == '/';
            size_t ns = closing ? 1 : 0;
            size_t ne = ns;
            while (ne < inner.size() &&
                   ((inner[ne] >= 'a' && inner[ne] <= 'z') ||
                    (inner[ne] >= 'A' && inner[ne] <= 'Z') ||
                    (inner[ne] >= '0' && inner[ne] <= '9'))) {
                ne++;
            }
            std::string name = inner.substr(ns, ne - ns);
            for (char& c : name) {
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            }

            if (name == "script") { inScript = !closing; i = end + 1; continue; }
            if (name == "style") { inStyle = !closing; i = end + 1; continue; }
            if (name == "head") { inHead = !closing; i = end + 1; continue; }

            if (!inScript && !inStyle && !inHead && isBlockTag(name)) {
                pendingBreak = true;
                flushSpace();
            }

            i = end + 1;
            continue;
        }

        if (inScript || inStyle || inHead) {
            i++;
            continue;
        }

        char c = html[i];
        if (isSpaceByte(c)) {
            pendingSpace = true;
            i++;
            continue;
        }

        flushSpace();
        out += c;
        i++;
    }

    // 收尾：去掉尾随空白
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();

    return decodeEntities(out);
}

std::string firstTagText(const std::string& xml, const std::string& tag) {
    std::string openPat = "<" + tag;
    size_t p = 0;
    while ((p = xml.find(openPat, p)) != std::string::npos) {
        size_t after = p + openPat.size();
        if (after < xml.size() && xml[after] != '>' && xml[after] != ' ' &&
            xml[after] != '\t' && xml[after] != '\n' && xml[after] != '\r' && xml[after] != '/') {
            p = after;
            continue;
        }
        size_t openEnd = xml.find('>', after);
        if (openEnd == std::string::npos) return "";
        if (openEnd > p && xml[openEnd - 1] == '/') return "";   // 空元素

        std::string closePat = "</" + tag + ">";
        size_t closePos = xml.find(closePat, openEnd + 1);
        if (closePos == std::string::npos) return "";

        std::string inner = xml.substr(openEnd + 1, closePos - openEnd - 1);
        std::string text = htmlToText(inner);
        if (!text.empty()) return text;
        p = closePos + closePat.size();
    }
    return "";
}

std::vector<std::string> allTagText(const std::string& xml, const std::string& tag) {
    std::vector<std::string> out;
    std::string openPat = "<" + tag;
    std::string closePat = "</" + tag + ">";

    size_t p = 0;
    while ((p = xml.find(openPat, p)) != std::string::npos) {
        size_t after = p + openPat.size();
        if (after < xml.size() && xml[after] != '>' && xml[after] != ' ' &&
            xml[after] != '\t' && xml[after] != '\n' && xml[after] != '\r' && xml[after] != '/') {
            p = after;
            continue;
        }
        size_t openEnd = xml.find('>', after);
        if (openEnd == std::string::npos) break;
        if (openEnd > p && xml[openEnd - 1] == '/') { p = openEnd + 1; continue; }

        size_t closePos = xml.find(closePat, openEnd + 1);
        if (closePos == std::string::npos) break;

        out.push_back(xml.substr(openEnd + 1, closePos - openEnd - 1));
        p = closePos + closePat.size();
    }

    return out;
}

} // namespace markup
