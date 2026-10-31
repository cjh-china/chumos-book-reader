#include "parser.h"
#include "zip_reader.h"
#include "markup_util.h"
#include "fs_util.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <vector>

namespace {

std::string toLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

// 取标签属性值：attr="..." 或 attr='...'
std::string getAttr(const std::string& tag, const std::string& attr) {
    std::string lower = toLower(tag);
    std::string key = toLower(attr);
    size_t p = 0;
    while ((p = lower.find(key, p)) != std::string::npos) {
        if (p > 0) {
            char prev = lower[p - 1];
            if (std::isalnum((unsigned char)prev) || prev == '_' || prev == '-') { p += key.size(); continue; }
        }
        size_t q = p + key.size();
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t' || tag[q] == '\n' || tag[q] == '\r')) q++;
        if (q >= tag.size() || tag[q] != '=') { p += key.size(); continue; }
        q++;
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t' || tag[q] == '\n' || tag[q] == '\r')) q++;
        if (q >= tag.size()) return "";
        char quote = tag[q];
        if (quote != '"' && quote != '\'') return "";
        q++;
        size_t e = tag.find(quote, q);
        if (e == std::string::npos) return "";
        return tag.substr(q, e - q);
    }
    return "";
}

// 找到 XML 中所有 <name ...> 的完整标签文本（含尖括号）
std::vector<std::string> findTags(const std::string& xml, const std::string& name) {
    std::vector<std::string> out;
    std::string pat = "<" + name;
    size_t p = 0;
    while ((p = xml.find(pat, p)) != std::string::npos) {
        size_t after = p + pat.size();
        if (after < xml.size()) {
            char c = xml[after];
            if (c != '>' && c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '/') {
                p = after;
                continue;
            }
        }
        size_t e = xml.find('>', after);
        if (e == std::string::npos) break;
        out.push_back(xml.substr(p, e - p + 1));
        p = e + 1;
    }
    return out;
}

// URL 解码（%20 等）并去掉 #fragment 与 query
std::string decodeHref(std::string href) {
    std::string out;
    for (size_t i = 0; i < href.size(); i++) {
        if (href[i] == '%' && i + 2 < href.size()) {
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hexv(href[i + 1]);
            int l = hexv(href[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        out += href[i];
    }
    size_t frag = out.find('#');
    if (frag != std::string::npos) out = out.substr(0, frag);
    return out;
}

// 相对路径解析：base 是目录（可能为空），rel 是相对 href
std::string resolvePath(const std::string& baseDir, const std::string& rel) {
    std::string joined = baseDir.empty() ? rel : (baseDir + "/" + rel);
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= joined.size(); i++) {
        if (i == joined.size() || joined[i] == '/') {
            if (cur == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += joined[i];
        }
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += "/";
        out += parts[i];
    }
    return out;
}

std::string dirOf(const std::string& path) {
    size_t p = path.find_last_of('/');
    if (p == std::string::npos) return "";
    return path.substr(0, p);
}

} // namespace

bool Parser::parseEPUB(const std::string& filename, Book& book) {
    zip::Reader zr;
    if (!zr.open(filename)) {
        book.title = fsutil::stem(filename);
        book.content.push_back("无法打开 EPUB：" + zr.lastError());
        return true;
    }

    std::string container;
    if (!zr.read("META-INF/container.xml", container)) {
        book.title = fsutil::stem(filename);
        book.content.push_back("EPUB 缺少 META-INF/container.xml");
        return true;
    }

    // 定位 OPF
    std::string opfPath;
    for (const auto& tag : findTags(container, "rootfile")) {
        std::string p = getAttr(tag, "full-path");
        if (!p.empty()) { opfPath = decodeHref(p); break; }
    }
    if (opfPath.empty()) {
        book.title = fsutil::stem(filename);
        book.content.push_back("EPUB 中找不到 OPF 文件");
        return true;
    }

    std::string opf;
    if (!zr.read(opfPath, opf)) {
        book.title = fsutil::stem(filename);
        book.content.push_back("无法读取 OPF：" + zr.lastError());
        return true;
    }

    const std::string opfDir = dirOf(opfPath);

    // 元信息
    book.title = markup::firstTagText(opf, "dc:title");
    if (book.title.empty()) book.title = fsutil::stem(filename);
    book.author = markup::firstTagText(opf, "dc:creator");

    // manifest: id -> href
    std::map<std::string, std::string> manifest;
    for (const auto& tag : findTags(opf, "item")) {
        std::string id = getAttr(tag, "id");
        std::string href = getAttr(tag, "href");
        if (!id.empty() && !href.empty()) manifest[id] = decodeHref(href);
    }

    // spine 顺序
    std::vector<std::string> spineHrefs;
    for (const auto& tag : findTags(opf, "itemref")) {
        std::string idref = getAttr(tag, "idref");
        auto it = manifest.find(idref);
        if (it != manifest.end()) spineHrefs.push_back(resolvePath(opfDir, it->second));
    }

    if (spineHrefs.empty()) {
        // 退化：manifest 里所有 xhtml/html 按名字排序
        for (const auto& kv : manifest) {
            std::string low = toLower(kv.second);
            if (low.size() > 5 && (low.find(".xhtml") != std::string::npos ||
                                   low.find(".html") != std::string::npos)) {
                spineHrefs.push_back(resolvePath(opfDir, kv.second));
            }
        }
        std::sort(spineHrefs.begin(), spineHrefs.end());
    }

    // 目录标题：toc.ncx（spine 的 toc 属性 → manifest id → ncx）
    std::map<std::string, std::string> tocTitles;   // 文件路径 -> 标题
    std::string ncxPath;
    for (const auto& tag : findTags(opf, "spine")) {
        std::string tocId = getAttr(tag, "toc");
        auto it = manifest.find(tocId);
        if (it != manifest.end()) ncxPath = resolvePath(opfDir, it->second);
        break;
    }
    if (ncxPath.empty()) {
        for (const auto& kv : manifest) {
            if (toLower(kv.second).find(".ncx") != std::string::npos) {
                ncxPath = resolvePath(opfDir, kv.second);
                break;
            }
        }
    }
    if (!ncxPath.empty()) {
        std::string ncx;
        if (zr.read(ncxPath, ncx)) {
            for (const auto& nav : markup::allTagText(ncx, "navPoint")) {
                std::string label = markup::firstTagText(nav, "text");
                std::string src = getAttr(nav, "src");
                for (const auto& ct : findTags(nav, "content")) {
                    std::string s = getAttr(ct, "src");
                    if (!s.empty()) { src = s; break; }
                }
                if (label.empty() || src.empty()) continue;
                std::string full = resolvePath(ncxPath.find('/') != std::string::npos ? dirOf(ncxPath) : opfDir,
                                               decodeHref(src));
                if (tocTitles.find(full) == tocTitles.end()) tocTitles[full] = label;
            }
        }
    }

    // 正文
    for (const auto& path : spineHrefs) {
        std::string raw;
        if (!zr.read(path, raw)) continue;

        std::string text = markup::htmlToText(raw);
        if (text.empty()) continue;

        std::string chapterTitle;
        auto t = tocTitles.find(path);
        if (t != tocTitles.end()) chapterTitle = t->second;

        bool startRecorded = false;
        std::string line;
        auto flush = [&]() {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            book.content.push_back(line);
            if (!startRecorded && !line.empty()) {
                std::string title = chapterTitle.empty() ? line : chapterTitle;
                if (title.size() > 180) {
                    // 按 UTF-8 字符截断
                    size_t cut = 0;
                    int cnt = 0;
                    while (cut < title.size() && cnt < 57) {
                        unsigned char c = (unsigned char)title[cut];
                        int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
                        cut += len;
                        cnt++;
                    }
                    title = title.substr(0, cut) + "...";
                }
                book.chapters.push_back(title);
                book.chapterStarts.push_back((int)book.content.size() - 1);
                startRecorded = true;
            }
            line.clear();
        };

        for (char c : text) {
            if (c == '\n') flush();
            else line += c;
        }
        if (!line.empty()) flush();

        // 章节之间留一个空行
        if (!book.content.empty() && !book.content.back().empty()) {
            book.content.push_back("");
        }
    }

    if (book.content.empty()) {
        book.content.push_back("（EPUB 中没有可提取的文本）");
    }

    return true;
}
