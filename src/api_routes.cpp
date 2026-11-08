#include "api_routes.h"

#include "config.h"
#include "fs_util.h"
#include "mini_json.h"
#include "parser.h"
#include "reader.h"

#ifdef HAS_CURL
#include "fanqie_downloader.h"
#include "sonovel.h"
#endif

#ifdef HAS_MUSIC
#include "music.h"
#include "online_music.h"
#include "qrcodegen.hpp"
#endif

#ifdef HAS_GUI
#include "gui.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace api {

namespace {

// ==================== 共享状态 ====================
// HTTP 服务是单线程串行处理的，这里的锁主要用于防御未来引入并发。
struct State {
    std::mutex mu;
    Config cfg;
    Book book;                 // 当前打开的书
    std::string bookPath;      // 当前书的路径
    bool loaded = false;
    int linesPerPage = 30;     // 与 TUI 默认值保持一致
};

State& state() {
    static State s;
    return s;
}

bool ensureConfig() {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.cfg.path().empty()) return true;
    return st.cfg.load();
}

// 书库目录：exe 同级 downloads/
std::string downloadDir() {
    std::string base = fsutil::exeDir();
    if (base.empty()) return "";
    return fsutil::join(base, "downloads");
}

// 把 Book 的分页信息算出来（不依赖 TUI 宽度：按标点/宽度粗略换行）
void ensureWrapped(Book& book, int width) {
    if (!book.wrappedContent.empty()) return;
    if (width < 20) width = 20;

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
        if (line.empty()) { book.wrappedContent.push_back(""); continue; }

        std::string current;
        int used = 0;
        size_t i = 0;
        while (i < line.size()) {
            unsigned char c = (unsigned char)line[i];
            int len = (c & 0x80) == 0 ? 1
                    : (c & 0xE0) == 0xC0 ? 2
                    : (c & 0xF0) == 0xE0 ? 3
                    : (c & 0xF8) == 0xF0 ? 4 : 1;
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

// 从查询串取整数，缺省返回 def
int queryInt(const Request& req, const std::string& key, int def) {
    auto it = req.query.find(key);
    if (it == req.query.end() || it->second.empty()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

std::string queryStr(const Request& req, const std::string& key, const std::string& def = "") {
    auto it = req.query.find(key);
    return it == req.query.end() ? def : it->second;
}

// 极简 JSON 字段提取：从请求体里取 "key": "value"
std::string bodyString(const std::string& body, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return "";
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == '\n' || body[p] == '\r')) p++;
    if (p >= body.size() || body[p] != '"') return "";
    p++;
    std::string out;
    while (p < body.size()) {
        char c = body[p];
        if (c == '\\' && p + 1 < body.size()) {
            char n = body[p + 1];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'r') out += '\r';
            else if (n == '"') out += '"';
            else if (n == '\\') out += '\\';
            else out += n;
            p += 2;
            continue;
        }
        if (c == '"') break;
        out += c;
        p++;
    }
    return out;
}

// ==================== 响应工具 ====================

Response okJson(const Json& j) {
    return Response::json(j.str());
}

Response badRequest(const std::string& msg) {
    return Response::error(msg, 400);
}

Response notFound(const std::string& msg) {
    return Response::error(msg, 404);
}

// ==================== 端点实现 ====================

Response handleHealth(const Request&) {
    Json j;
    j.beginObject();
    j.key("status").value("ok");
    j.key("version").value("1.0.0");
    std::vector<std::string> caps;
    caps.push_back("reading");
    caps.push_back("search");
    caps.push_back("library-detail");
#ifdef HAS_CURL
    caps.push_back("fanqie");
    caps.push_back("fanqie-jobs");
    caps.push_back("fanqie-updates");
    caps.push_back("fanqie-cover");
    caps.push_back("sonovel");
    caps.push_back("online-books");
#endif
#ifdef HAS_MUSIC
    caps.push_back("music");
    caps.push_back("online-music");
#endif
#if defined(HAS_GUI) || defined(HAS_MUSIC)
    caps.push_back("music-file");
#endif
#ifdef HAS_GUI
    caps.push_back("gui");
#endif
    j.key("capabilities");
    j.beginArray();
    for (const auto& c : caps) j.value(c);
    j.endArray();
    j.endObject();
    return okJson(j);
}

// 汇总书库文件：默认扫描 downloads/ + 配置登记目录，dir 非空时只扫描该目录。
// 供 /api/library、/api/search 复用，保证两处看到的书库完全一致。
bool gatherLibraryFiles(const std::string& dir, int maxDepth,
                        std::vector<std::string>& files,
                        std::string& err, int& status) {
    files.clear();
    if (!ensureConfig()) { err = "配置加载失败"; status = 500; return false; }
    if (!dir.empty()) {
        if (!fsutil::isDirectory(dir)) { err = "目录不存在: " + dir; status = 400; return false; }
        files = Config::scanDir(dir, maxDepth);
    } else {
        std::string dl = downloadDir();
        if (!dl.empty() && fsutil::isDirectory(dl)) {
            files = Config::scanDir(dl, maxDepth);
        }
        for (const auto& d : state().cfg.dirs()) {
            if (!fsutil::isDirectory(d)) continue;
            auto more = Config::scanDir(d, maxDepth);
            files.insert(files.end(), more.begin(), more.end());
        }
    }

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return true;
}

// 把一本书的常用字段写进 JSON（书库列表 / 搜索结果共用）。
void writeLibraryBook(Json& j, const std::string& f) {
    j.beginObject();
    j.key("path").value(f);
    j.key("name").value(fsutil::filename(f));
    j.key("size").value((long long)fsutil::fileSize(f));
    j.key("mtime").value((long long)fsutil::lastWriteMs(f));
    auto p = state().cfg.getProgress(f);
    j.key("percent").value(p.percent);
    j.key("lastLine").value(p.line);
    j.key("updatedAt").value(p.updatedAt);
    j.endObject();
}

Response handleLibrary(const Request& req) {
    std::vector<std::string> files;
    std::string err;
    int status = 500;
    if (!gatherLibraryFiles(queryStr(req, "dir"), queryInt(req, "depth", 4),
                            files, err, status)) {
        return Response::error(err, status);
    }

    Json j;
    j.beginObject();
    j.key("count").value((int)files.size());
    j.key("books");
    j.beginArray();
    for (const auto& f : files) writeLibraryBook(j, f);
    j.endArray();
    j.endObject();
    return okJson(j);
}

// 书籍详情：按需解析单本书，不改变当前正在阅读的书。
Response handleLibraryDetail(const Request& req) {
    std::string path = queryStr(req, "path");
    if (path.empty()) return badRequest("缺少参数 path");
    if (!fsutil::isFile(path)) return notFound("文件不存在: " + path);

    Book book;
    if (!Parser::parse(path, book)) return Response::error("解析失败: " + path, 500);

    size_t chars = 0;
    for (const auto& line : book.content) chars += line.size();
    std::string intro;
    for (const auto& line : book.content) {
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || Parser::isChapterTitle(line)) continue;
        if (!intro.empty()) intro += "  ";
        intro += line.substr(b);
        if (intro.size() >= 180) break;
    }
    if (intro.size() >= 180) intro += "…";

    if (!ensureConfig()) return Response::error("配置加载失败", 500);
    auto p = state().cfg.getProgress(path);

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("path").value(path);
    j.key("name").value(fsutil::filename(path));
    j.key("title").value(book.title);
    j.key("author").value(book.author);
    j.key("intro").value(intro);
    j.key("size").value((long long)fsutil::fileSize(path));
    j.key("mtime").value((long long)fsutil::lastWriteMs(path));
    j.key("lines").value((int)book.content.size());
    j.key("chars").value((int)chars);
    j.key("chapters").value((int)book.chapters.size());
    j.key("percent").value(p.percent);
    j.key("lastLine").value(p.line);
    j.key("toc");
    j.beginArray();
    for (size_t i = 0; i < book.chapters.size() && i < 12; i++) {
        j.beginObject();
        j.key("index").value((int)i);
        j.key("title").value(book.chapters[i]);
        j.key("line").value(i < book.chapterStarts.size() ? book.chapterStarts[i] : 0);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleBookOpen(const Request& req) {
    std::string path = req.method == "POST" ? bodyString(req.body, "path")
                                            : queryStr(req, "path");
    if (path.empty()) return badRequest("缺少参数 path");
    if (!fsutil::isFile(path)) return notFound("文件不存在: " + path);

    State& st = state();

    Book fresh;
    if (!Parser::parse(path, fresh)) return Response::error("解析失败: " + path, 500);

    std::lock_guard<std::mutex> lock(st.mu);
    st.book = std::move(fresh);
    st.bookPath = path;
    st.loaded = true;
    ensureWrapped(st.book, 80);

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("path").value(path);
    j.key("title").value(st.book.title);
    j.key("author").value(st.book.author);
    j.key("lines").value((int)st.book.content.size());
    j.key("wrappedLines").value((int)st.book.wrappedContent.size());
    j.key("chapters").value((int)st.book.chapters.size());
    j.endObject();
    return okJson(j);
}

Response handleBookInfo(const Request&) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.loaded) return badRequest("尚未打开任何书，请先调用 /api/book/open");

    int total = (int)st.book.wrappedContent.size();
    int pageSize = st.linesPerPage;
    int pages = pageSize > 0 ? (total + pageSize - 1) / pageSize : 0;

    Json j;
    j.beginObject();
    j.key("path").value(st.bookPath);
    j.key("title").value(st.book.title);
    j.key("author").value(st.book.author);
    j.key("lines").value((int)st.book.content.size());
    j.key("wrappedLines").value(total);
    j.key("pageSize").value(pageSize);
    j.key("pages").value(pages);
    j.key("chapters").value((int)st.book.chapters.size());
    j.endObject();
    return okJson(j);
}

// UTF-8 字符数：只统计非续字节，中文按一个字计，避免按字节夸大篇幅。
size_t utf8CharCount(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

// 章节边界统一按「原始段落行号」计算：
// [start, end)，end 为下一章标题所在行，最后一章则到全文末尾。
bool hasUsableToc(const Book& book) {
    return !book.chapters.empty() && book.chapterStarts.size() >= book.chapters.size();
}
int chapterStartAt(const Book& book, size_t i) {
    if (i >= book.chapterStarts.size()) return 0;
    int v = book.chapterStarts[i];
    return v < 0 ? 0 : v;
}
int chapterEndAt(const Book& book, size_t i) {
    int total = (int)book.content.size();
    int start = chapterStartAt(book, i);
    if (i + 1 < book.chapterStarts.size()) {
        int next = book.chapterStarts[i + 1];
        if (next > start && next <= total) return next;
    }
    return total > start ? total : start;
}
size_t chapterCharCount(const Book& book, int start, int end) {
    size_t n = 0;
    for (int i = start; i < end && i < (int)book.content.size(); i++) {
        n += utf8CharCount(book.content[i]);
    }
    return n;
}
int chapterMinutes(size_t chars) {
    // 按每分钟 400 字估算，至少 1 分钟
    return (int)((chars + 399) / 400 < 1 ? 1 : (chars + 399) / 400);
}

Response handleBookToc(const Request&) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.loaded) return badRequest("尚未打开任何书");

    const size_t n = st.book.chapters.size();
    const int total = (int)st.book.content.size();
    const bool usable = hasUsableToc(st.book);

    Json j;
    j.beginObject();
    j.key("count").value((int)n);
    j.key("totalLines").value(total);
    j.key("title").value(st.book.title);
    j.key("chapters");
    j.beginArray();
    for (size_t i = 0; i < n; i++) {
        int start = usable ? chapterStartAt(st.book, i) : 0;
        int end = usable ? chapterEndAt(st.book, i) : total;
        size_t chars = chapterCharCount(st.book, start, end);
        j.beginObject();
        j.key("index").value((int)i);
        j.key("title").value(st.book.chapters[i]);
        j.key("line").value(start);
        j.key("end").value(end);
        j.key("paragraphs").value(end - start);
        j.key("chars").value((int)chars);
        j.key("minutes").value(chapterMinutes(chars));
        j.key("percent").value(total > 0 ? (int)(start * 100LL / total) : 0);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

// 按章节阅读：返回某一章的完整原始段落，并给出上一章 / 下一章定位信息。
// index=N 指定章节；也可用 line=N 定位所在章节。limit 限制单次返回的段落数。
Response handleBookChapter(const Request& req) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.loaded) return badRequest("尚未打开任何书");

    const int total = (int)st.book.content.size();
    const int count = (int)st.book.chapters.size();
    const bool usable = hasUsableToc(st.book);

    int index = queryInt(req, "index", -1);
    int line = queryInt(req, "line", -1);
    if (line >= 0 && usable) {
        // 找到包含该行的最后一章
        int lo = 0, hi = count - 1, ans = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (chapterStartAt(st.book, (size_t)mid) <= line) {
                ans = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        index = ans;
    }
    if (index < 0) index = 0;
    if (usable && index >= count) return badRequest("章节序号越界");

    int start = 0;
    int end = total;
    std::string title = st.book.title.empty() ? "全文" : st.book.title;
    if (usable) {
        start = chapterStartAt(st.book, (size_t)index);
        end = chapterEndAt(st.book, (size_t)index);
        title = st.book.chapters[(size_t)index];
    }

    int limit = queryInt(req, "limit", 4000);
    if (limit < 1) limit = 4000;
    if (limit > 10000) limit = 10000;
    int outEnd = end;
    bool truncated = false;
    if (outEnd - start > limit) {
        outEnd = start + limit;
        truncated = true;
    }
    size_t chars = chapterCharCount(st.book, start, end);

    int prevIndex = usable ? index - 1 : -1;
    int nextIndex = usable && index + 1 < count ? index + 1 : -1;

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("count").value(count);
    j.key("index").value(index);
    j.key("title").value(title);
    j.key("book").value(st.book.title);
    j.key("start").value(start);
    j.key("end").value(end);
    j.key("total").value(total);
    j.key("paragraphs").value(end - start);
    j.key("chars").value((int)chars);
    j.key("minutes").value(chapterMinutes(chars));
    j.key("fallback").value(!usable);
    j.key("truncated").value(truncated);
    j.key("prevIndex").value(prevIndex);
    j.key("prevTitle").value(prevIndex >= 0 ? st.book.chapters[(size_t)prevIndex] : "");
    j.key("prevLine").value(prevIndex >= 0 ? chapterStartAt(st.book, (size_t)prevIndex) : -1);
    j.key("nextIndex").value(nextIndex);
    j.key("nextTitle").value(nextIndex >= 0 ? st.book.chapters[(size_t)nextIndex] : "");
    j.key("nextLine").value(nextIndex >= 0 ? chapterStartAt(st.book, (size_t)nextIndex) : -1);
    j.key("lines");
    j.beginArray();
    for (int i = start; i < outEnd; i++) j.value(st.book.content[i]);
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleBookContent(const Request& req) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.loaded) return badRequest("尚未打开任何书");

    // raw=1：返回未经折行的原始段落，供 GUI 前端自行排版（首行缩进、字号、行距）。
    // 默认行为与 TUI / 既有客户端一致，仍返回按 80 列折行后的文本行。
    // 目录接口 /api/book/toc 的 line 是原始段落行号，raw 模式下可直接对齐。
    bool raw = queryInt(req, "raw", 0) != 0;
    const std::vector<std::string>& source =
        raw ? st.book.content : st.book.wrappedContent;
    int pageSize = raw ? 40 : st.linesPerPage;
    int page = queryInt(req, "page", 0);
    int start = 0;
    int end = 0;

    if (req.query.count("line") || req.query.count("offset")) {
        // 按行号取
        start = queryInt(req, "line", queryInt(req, "offset", 0));
        int count = queryInt(req, "count", pageSize);
        if (start < 0) start = 0;
        end = start + (count > 0 ? count : pageSize);
    } else {
        // 按页取
        if (page < 0) page = 0;
        start = page * pageSize;
        end = start + pageSize;
    }

    int total = (int)source.size();
    if (start > total) start = total;
    if (end > total) end = total;

    Json j;
    j.beginObject();
    j.key("total").value(total);
    j.key("pageSize").value(pageSize);
    j.key("raw").value(raw);
    j.key("start").value(start);
    j.key("end").value(end);
    j.key("lines");
    j.beginArray();
    for (int i = start; i < end; i++) j.value(source[i]);
    j.endArray();
    j.endObject();
    return okJson(j);
}

// 阅读进度：复用 Config 里 TUI 也在用的 readProgress，GUI 与 TUI 共享同一份记录。
// POST 请求体里的数值用字符串传递（本文件的 bodyString 只抽取带引号的值）。
Response handleBookProgress(const Request& req) {
    if (!ensureConfig()) return Response::error("配置加载失败", 500);

    std::string path = req.method == "POST" ? bodyString(req.body, "path")
                                            : queryStr(req, "path");
    if (path.empty()) path = state().bookPath;
    if (path.empty()) return badRequest("缺少参数 path");

    if (req.method == "POST") {
        int line = 0, percent = 0;
        try { line = std::stoi(bodyString(req.body, "line")); } catch (...) {}
        try { percent = std::stoi(bodyString(req.body, "percent")); } catch (...) {}
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        state().cfg.setProgress(path, line, percent);
    }

    // ReadProgress / Bookmark 是 Config 的私有嵌套类型，用 auto 推导避免显式命名。
    auto p = state().cfg.getProgress(path);
    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("path").value(path);
    j.key("line").value(p.line);
    j.key("percent").value(p.percent);
    j.key("updatedAt").value(p.updatedAt);
    j.endObject();
    return okJson(j);
}

// 书签：GET 列出，POST 通过 action=add|remove 增删。
Response handleBookBookmarks(const Request& req) {
    if (!ensureConfig()) return Response::error("配置加载失败", 500);

    std::string path = req.method == "POST" ? bodyString(req.body, "path")
                                            : queryStr(req, "path");
    if (path.empty()) path = state().bookPath;
    if (path.empty()) return badRequest("缺少参数 path");

    if (req.method == "POST") {
        std::string action = bodyString(req.body, "action");
        int line = 0;
        try { line = std::stoi(bodyString(req.body, "line")); } catch (...) {}
        if (action == "add") {
            state().cfg.addBookmark(path, line, bodyString(req.body, "note"));
        } else if (action == "remove") {
            state().cfg.removeBookmark(path, line);
        } else {
            return badRequest("action 只支持 add / remove");
        }
    }

    const auto& marks = state().cfg.getBookmarks(path);
    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("path").value(path);
    j.key("count").value((int)marks.size());
    j.key("bookmarks");
    j.beginArray();
    for (const auto& bm : marks) {
        j.beginObject();
        j.key("line").value(bm.line);
        j.key("note").value(bm.note);
        j.key("createdAt").value(bm.createdAt);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleBookSearch(const Request& req) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.loaded) return badRequest("尚未打开任何书");

    std::string q = queryStr(req, "q");
    if (q.empty()) return badRequest("缺少参数 q");
    int limit = queryInt(req, "limit", 200);

    // 大小写不敏感的子串查找（ASCII 折叠，中文按字节匹配）
    auto fold = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return s;
    };
    std::string needle = fold(q);
    // raw=1 时搜索原始段落并返回原始行号，便于 GUI 直接跳转；默认保持折行行号。
    bool raw = queryInt(req, "raw", 0) != 0;
    const std::vector<std::string>& source =
        raw ? st.book.content : st.book.wrappedContent;

    Json j;
    j.beginObject();
    j.key("query").value(q);
    j.key("raw").value(raw);
    j.key("matches");
    j.beginArray();
    int found = 0;
    for (size_t i = 0; i < source.size() && found < limit; i++) {
        if (fold(source[i]).find(needle) == std::string::npos) continue;
        j.beginObject();
        j.key("line").value((int)i);
        j.key("text").value(source[i]);
        j.endObject();
        found++;
    }
    j.endArray();
    j.key("count").value(found);
    j.endObject();
    return okJson(j);
}

// ==================== 音乐界面：本地音频服务 ====================
// 浏览器 <audio> 需要 206 + Content-Range 才能拖动进度。
// 只放行 downloads/music 下的音频文件，避免把本机任意文件暴露给页面。
#if defined(HAS_GUI) || defined(HAS_MUSIC)

std::string musicRootDir() {
    return fsutil::normalize(fsutil::join(fsutil::join(fsutil::exeDir(), "downloads"), "music"));
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool isAudioFilePath(const std::string& path) {
    std::string e = fsutil::extension(path);
    return e == "mp3" || e == "flac" || e == "wav" ||
           e == "ogg" || e == "m4a" || e == "aac";
}

std::string audioContentType(const std::string& path) {
    std::string e = fsutil::extension(path);
    if (e == "mp3") return "audio/mpeg";
    if (e == "flac") return "audio/flac";
    if (e == "wav") return "audio/wav";
    if (e == "ogg") return "audio/ogg";
    if (e == "m4a" || e == "aac") return "audio/mp4";
    return "application/octet-stream";
}

bool isInsideMusicDir(const std::string& path, const std::string& dir) {
    auto unify = [](std::string s) {
        for (char& c : s) if (c == '\\') c = '/';
        return s;
    };
    std::string p = unify(fsutil::normalize(path));
    std::string d = unify(fsutil::normalize(dir));
    if (d.empty() || p.size() <= d.size()) return false;
    if (p.find("..") != std::string::npos) return false;   // 拒绝路径穿越
    if (lowerAscii(p.substr(0, d.size())) != lowerAscii(d)) return false;
    return p[d.size()] == '/';
}

Response handleMusicFile(const Request& req) {
    std::string path = queryStr(req, "path");
    if (path.empty()) return badRequest("缺少参数 path");
    std::string root = musicRootDir();
    std::string full = fsutil::normalize(path);
    if (!isInsideMusicDir(full, root)) return notFound("只允许播放 downloads/music 下的音频文件");
    if (!isAudioFilePath(full) || !fsutil::isFile(full)) return notFound("音频文件不存在");

    std::string data;
    if (!fsutil::readAll(full, data)) return Response::error("读取音频文件失败", 500);
    long long size = (long long)data.size();
    if (size == 0) return Response::error("音频文件为空", 500);

    long long start = 0, end = size - 1;
    bool ranged = false;
    auto it = req.headers.find("range");
    if (it != req.headers.end()) {
        std::string v = it->second;
        if (v.compare(0, 6, "bytes=") == 0) {
            std::string spec = v.substr(6);
            size_t dash = spec.find('-');
            std::string a = spec.substr(0, dash);
            std::string b = dash == std::string::npos ? "" : spec.substr(dash + 1);
            try {
                if (!a.empty()) start = std::stoll(a);
                else if (!b.empty()) start = size - std::stoll(b);
            } catch (...) { start = 0; }
            if (!a.empty() && !b.empty()) {
                try { end = std::stoll(b); } catch (...) {}
            }
            ranged = true;
        }
    }
    if (start < 0) start = 0;
    if (start >= size) start = size - 1;
    if (end >= size) end = size - 1;
    if (end < start) end = start;
    const long long kChunk = 1024 * 1024;   // 单次最多 1 MB，避免长时间阻塞其它 API 请求
    if (end - start + 1 > kChunk) end = start + kChunk - 1;
    if (!ranged && size > kChunk) ranged = true;   // 大文件无 Range 时也分块

    Response res;
    res.status = ranged ? 206 : 200;
    res.contentType = audioContentType(full);
    res.body.assign(data, (size_t)start, (size_t)(end - start + 1));
    res.headers["Accept-Ranges"] = "bytes";
    res.headers["Cache-Control"] = "no-store";
    if (ranged) {
        res.headers["Content-Range"] = "bytes " + std::to_string(start) + "-" +
                                       std::to_string(end) + "/" + std::to_string(size);
    }
    return res;
}
#endif

// 未编译音乐模块时，GUI 仍可播放 downloads/music 下的本地音频。
#if defined(HAS_GUI) && !defined(HAS_MUSIC)
Response handleMusicLibrary(const Request& req) {
    std::string dir = queryStr(req, "dir");
    if (dir.empty()) dir = musicRootDir();
    Json j;
    j.beginObject();
    j.key("dir").value(dir);
    if (!fsutil::isDirectory(dir)) {
        j.key("count").value(0);
        j.key("songs");
        j.beginArray();
        j.endArray();
        j.key("note").value("目录不存在，把音频文件放进 downloads/music 即可");
        j.endObject();
        return okJson(j);
    }
    auto files = fsutil::listFilesRecursive(dir, 4);
    std::vector<std::string> songs;
    for (const auto& f : files) if (isAudioFilePath(f)) songs.push_back(f);
    std::sort(songs.begin(), songs.end());
    j.key("count").value((int)songs.size());
    j.key("songs");
    j.beginArray();
    for (const auto& f : songs) {
        std::string stem = fsutil::stem(fsutil::filename(f));
        std::string title = stem, artist = "";
        size_t sep = stem.find(" - ");
        if (sep != std::string::npos) {
            artist = stem.substr(0, sep);
            title = stem.substr(sep + 3);
        }
        j.beginObject();
        j.key("path").value(f);
        j.key("title").value(title);
        j.key("artist").value(artist);
        j.key("album").value("");
        j.key("duration").value(0);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}
#endif

#ifdef HAS_CURL

Response handleFanqieSearch(const Request& req) {
    std::string q = queryStr(req, "q");
    if (q.empty()) return badRequest("缺少参数 q");

    FanqieDownloader dl(4);
    if (!dl.ensureService()) return Response::error("番茄下载器服务不可用", 503);

    auto books = dl.searchBooks(q);
    Json j;
    j.beginObject();
    j.key("count").value((int)books.size());
    j.key("books");
    j.beginArray();
    for (const auto& b : books) {
        j.beginObject();
        j.key("bookId").value(b.book_id);
        j.key("name").value(b.book_name);
        j.key("author").value(b.author);
        j.key("score").value(b.score);
        j.key("abstract").value(b.abstract);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleFanqieDownload(const Request& req) {
    std::string bookId = req.method == "POST" ? bodyString(req.body, "bookId")
                                              : queryStr(req, "bookId");
    std::string name = req.method == "POST" ? bodyString(req.body, "name")
                                            : queryStr(req, "name");
    if (bookId.empty()) return badRequest("缺少参数 bookId");

    FanqieDownloader dl(8);
    if (!dl.ensureService()) return Response::error("番茄下载器服务不可用", 503);

    std::string dir = downloadDir();
    if (dir.empty()) return Response::error("无法确定下载目录", 500);
    fsutil::createDirs(dir);

    std::string bookName = name.empty() ? bookId : name;
    if (!dl.downloadBook(bookId, bookName, dir)) {
        return Response::error("下载失败: " + bookName, 500);
    }

    // 下载后的文件名规则与 TUI 一致：sanitize(name) + ".txt"
    std::string file = fsutil::join(dir, FanqieDownloader::sanitizeName(bookName) + ".txt");

    Json j;
    j.beginObject();
    j.key("ok").value(fsutil::isFile(file));
    j.key("path").value(file);
    j.key("bookId").value(bookId);
    j.endObject();
    return okJson(j);
}

// ---------- 番茄下载器 2.4.x 能力 ----------

// 服务状态 + 当前下载配置
Response handleFanqieStatus(const Request&) {
    FanqieDownloader dl(2);
    FanqieDownloader::ServerStatus st = dl.status();
    if (!st.ok) return Response::error("番茄下载器服务不可用", 503);

    FanqieDownloader::WebConfig cfg = dl.webConfig();

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("version").value(st.version);
    j.key("saveDir").value(st.saveDir);
    j.key("locked").value(st.locked);
    j.key("docker").value(st.docker);
    j.key("format").value(cfg.novelFormat);
    j.key("audiobook").value(cfg.enableAudiobook);
    j.key("audiobookFormat").value(cfg.audiobookFormat);
    j.key("bulkFiles").value(cfg.bulkFiles);
    j.key("askFormat").value(cfg.askFormat);
    j.endObject();
    return okJson(j);
}

// 读取当前下载器配置（与 POST 同一端点，便于调试与前端回显）
Response handleFanqieConfigGet(const Request&) {
    FanqieDownloader dl(2);
    FanqieDownloader::WebConfig cfg = dl.webConfig();
    if (!cfg.valid) return Response::error("番茄下载器服务不可用", 503);

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("format").value(cfg.novelFormat);
    j.key("audiobook").value(cfg.enableAudiobook);
    j.key("audiobookFormat").value(cfg.audiobookFormat);
    j.key("bulkFiles").value(cfg.bulkFiles);
    j.key("askFormat").value(cfg.askFormat);
    j.endObject();
    return okJson(j);
}

// 写入下载配置（格式 / 有声书 / 分章）
Response handleFanqieConfigSet(const Request& req) {
    FanqieDownloader dl(2);
    FanqieDownloader::WebConfig cfg = dl.webConfig();
    if (!cfg.valid) return Response::error("番茄下载器服务不可用", 503);

    minijson::Value b = minijson::parse(req.body);
    std::string fmt = b["format"].toStringValue();
    if (!fmt.empty()) cfg.novelFormat = fmt;
    std::string abf = b["audiobookFormat"].toStringValue();
    if (!abf.empty()) cfg.audiobookFormat = abf;
    if (b.has("audiobook")) cfg.enableAudiobook = b["audiobook"].asBool();
    if (b.has("bulkFiles")) cfg.bulkFiles = b["bulkFiles"].asBool();

    std::string err;
    if (!dl.applyConfig(cfg, err)) {
        return Response::error(err.empty() ? "配置写入失败" : err, 502);
    }

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("format").value(cfg.novelFormat);
    j.key("audiobook").value(cfg.enableAudiobook);
    j.key("audiobookFormat").value(cfg.audiobookFormat);
    j.key("bulkFiles").value(cfg.bulkFiles);
    j.endObject();
    return okJson(j);
}

// 书籍预览：封面 / 章数 / 字数 / 在读 / 最新章
Response handleFanqiePreview(const Request& req) {
    std::string bookId = queryStr(req, "bookId");
    if (bookId.empty()) return badRequest("缺少参数 bookId");

    FanqieDownloader dl(2);
    FanqieDownloader::PreviewInfo p = dl.preview(bookId);
    if (!p.valid) return Response::error("获取书籍信息失败（书源可能超时）", 502);

    Json j;
    j.beginObject();
    j.key("bookId").value(p.bookId);
    j.key("name").value(p.name);
    j.key("author").value(p.author);
    j.key("category").value(p.category);
    j.key("abstract").value(p.description);
    j.key("tags").value(p.tags);
    j.key("score").value(p.score);
    j.key("readCount").value(p.readCountText);
    j.key("firstChapter").value(p.firstChapter);
    j.key("lastChapter").value(p.lastChapter);
    j.key("chapters").value(p.chapterCount);
    j.key("words").value(p.wordCount);
    j.key("finished").value(p.finished);
    j.key("cover").value("/api/fanqie/cover?bookId=" + api::urlEncode(bookId));
    j.endObject();
    return okJson(j);
}

// 封面代理（JPEG 字节流）
Response handleFanqieCover(const Request& req) {
    std::string bookId = queryStr(req, "bookId");
    if (bookId.empty()) return badRequest("缺少参数 bookId");

    FanqieDownloader dl(2);
    std::string bytes;
    if (!dl.fetchCover(bookId, bytes) || bytes.empty()) {
        return Response::error("封面不可用", 404);
    }

    Response r;
    r.status = 200;
    r.contentType = "image/jpeg";
    r.body = bytes;
    r.headers["Cache-Control"] = "public, max-age=86400";
    return r;
}

// 创建下载任务（异步，前端轮询 /api/fanqie/job）
Response handleFanqieJobStart(const Request& req) {
    minijson::Value b = minijson::parse(req.body);
    std::string bookId = b["bookId"].toStringValue();
    if (bookId.empty()) bookId = b["book_id"].toStringValue();
    if (bookId.empty()) bookId = queryStr(req, "bookId");
    if (bookId.empty()) return badRequest("缺少参数 bookId");

    FanqieDownloader::DownloadOptions opt;
    opt.format = b["format"].toStringValue();
    if (b.has("audiobook")) opt.audiobookMode = b["audiobook"].asBool() ? 1 : 0;
    opt.audiobookFormat = b["audiobookFormat"].toStringValue();
    opt.rangeStart = (int)b["rangeStart"].asNumber();
    opt.rangeEnd = (int)b["rangeEnd"].asNumber();

    FanqieDownloader dl(2);
    std::string err;
    int id = dl.startJob(bookId, opt, err);
    if (id <= 0) return Response::error(err.empty() ? "创建任务失败" : err, 502);

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("jobId").value(id);
    j.key("bookId").value(bookId);
    j.endObject();
    return okJson(j);
}

// 任务快照
Response handleFanqieJob(const Request& req) {
    int id = queryInt(req, "id", 0);
    if (id <= 0) return badRequest("缺少参数 id");

    FanqieDownloader dl(2);
    FanqieDownloader::JobInfo ji = dl.job(id);
    if (!ji.valid) return notFound("任务不存在");

    Json j;
    j.beginObject();
    j.key("id").value(ji.id);
    j.key("bookId").value(ji.bookId);
    j.key("title").value(ji.title);
    j.key("author").value(ji.author);
    j.key("state").value(ji.state);
    j.key("message").value(ji.message);
    j.key("saved").value(ji.savedChapters);
    j.key("total").value(ji.totalChapters);
    j.key("phase").value(ji.savePhase);
    j.key("done").value(ji.finished());
    j.key("formatOptions");
    j.beginArray();
    for (const auto& s : ji.formatOptions) j.value(s);
    j.endArray();
    j.key("bookNameOptions");
    j.beginArray();
    for (const auto& s : ji.bookNameOptions) j.value(s);
    j.endArray();
    j.endObject();
    return okJson(j);
}

// 任务操作：action=cancel / format / bookname
Response handleFanqieJobAction(const Request& req) {
    minijson::Value b = minijson::parse(req.body);
    int id = (int)b["id"].asNumber();
    if (id <= 0) id = queryInt(req, "id", 0);
    if (id <= 0) return badRequest("缺少参数 id");

    // action 既可以放在查询串，也可以放在请求体里
    std::string action = queryStr(req, "action");
    if (action.empty()) action = b["action"].toStringValue();
    std::string value = b["value"].toStringValue();
    if (value.empty()) value = queryStr(req, "value");

    FanqieDownloader dl(2);
    bool ok = false;
    if (action == "cancel") ok = dl.cancelJob(id);
    else if (action == "format") ok = dl.chooseFormat(id, value);
    else if (action == "bookname") ok = dl.chooseBookName(id, value);
    else return badRequest("未知操作: " + action);

    if (!ok) return Response::error("操作失败或任务已结束", 502);

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.endObject();
    return okJson(j);
}

// 服务端书库（含 epub / 有声书目录）
Response handleFanqieLibrary(const Request& req) {
    std::string path = queryStr(req, "path");
    std::string name = queryStr(req, "name");
    // 默认只读缓存（扫描是异步的，发起的当次拿不到结果）；start=true/1 才触发重扫
    std::string startRaw = queryStr(req, "start", "false");
    bool startScan = (startRaw == "true" || startRaw == "1");

    FanqieDownloader dl(2);
    bool running = false;
    int scanned = 0;
    auto items = dl.library(path, name, startScan, &running, &scanned);

    Json j;
    j.beginObject();
    j.key("path").value(path);
    j.key("running").value(running);
    j.key("scanned").value(scanned);
    j.key("count").value((int)items.size());
    j.key("items");
    j.beginArray();
    for (const auto& e : items) {
        j.beginObject();
        j.key("kind").value(e.kind);
        j.key("name").value(e.name);
        j.key("path").value(e.relPath);
        j.key("ext").value(e.ext);
        j.key("size").value((long long)e.size);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

// 任务完成后把产出文件取回本地 downloads/
Response handleFanqieImport(const Request& req) {
    minijson::Value b = minijson::parse(req.body);
    std::string bookId = b["bookId"].toStringValue();
    std::string name = b["name"].toStringValue();
    std::string format = b["format"].toStringValue();
    if (format.empty()) format = "txt";
    if (name.empty() && bookId.empty()) return badRequest("缺少参数 name / bookId");

    std::string dir = downloadDir();
    if (dir.empty()) return Response::error("无法确定下载目录", 500);

    FanqieDownloader dl(2);
    std::string err;
    std::string path = dl.importProduced(bookId, name, format, dir, err);
    if (path.empty()) {
        return Response::error(err.empty() ? "取回文件失败" : err, 502);
    }

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("path").value(path);
    j.key("format").value(format);
    j.endObject();
    return okJson(j);
}

// 追更扫描（start=1 触发新扫描，start=0 只读结果）
Response handleFanqieUpdates(const Request& req) {
    // 默认首次进入时触发扫描；轮询传 start=false（也兼容 0）只读状态
    std::string startRaw = queryStr(req, "start", "true");
    bool startScan = !(startRaw == "false" || startRaw == "0");

    FanqieDownloader dl(2);
    bool running = false;
    auto list = dl.updates(startScan, running);

    Json j;
    j.beginObject();
    j.key("running").value(running);
    j.key("count").value((int)list.size());
    j.key("updates");
    j.beginArray();
    for (const auto& e : list) {
        j.beginObject();
        j.key("bookId").value(e.bookId);
        j.key("name").value(e.bookName);
        j.key("folder").value(e.folder);
        j.key("local").value(e.localTotal);
        j.key("remote").value(e.remoteTotal);
        j.key("newCount").value(e.newCount);
        j.key("hasUpdate").value(e.hasUpdate);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

// 下载历史
Response handleFanqieHistory(const Request& req) {
    int limit = queryInt(req, "limit", 30);
    if (limit <= 0) limit = 30;

    FanqieDownloader dl(2);
    auto list = dl.history(limit);

    Json j;
    j.beginObject();
    j.key("count").value((int)list.size());
    j.key("items");
    j.beginArray();
    for (const auto& e : list) {
        j.beginObject();
        j.key("time").value(e.timestamp);
        j.key("bookId").value(e.bookId);
        j.key("name").value(e.bookName);
        j.key("author").value(e.author);
        j.key("status").value(e.status);
        j.key("progress").value(e.progressText);
        j.key("selected").value(e.selectedChapters);
        j.key("success").value(e.successChapters);
        j.key("failed").value(e.failedChapters);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleSoNovelSearch(const Request& req) {
    std::string q = queryStr(req, "q");
    if (q.empty()) return badRequest("缺少参数 q");

    SoNovel svc;
    if (!svc.isAvailable()) {
        bool autoStart = true;
        if (ensureConfig()) autoStart = state().cfg.autoStartSoNovel();
        if (!autoStart) return Response::error("so-novel 服务未运行（自动启动已关闭）", 503);
        std::string home;
        if (ensureConfig()) home = state().cfg.soNovelHome();
        if (!svc.ensureService(home, 25)) {
            return Response::error("无法启动 so-novel: " + svc.lastError(), 503);
        }
    }

    auto books = svc.search(q);
    Json j;
    j.beginObject();
    j.key("count").value((int)books.size());
    if (books.empty() && !svc.lastError().empty()) {
        j.key("error").value(svc.lastError());
    }
    j.key("books");
    j.beginArray();
    for (const auto& b : books) {
        j.beginObject();
        j.key("sourceId").value(b.sourceId);
        j.key("sourceName").value(b.sourceName);
        j.key("url").value(b.url);
        j.key("name").value(b.bookName);
        j.key("author").value(b.author);
        j.key("category").value(b.category);
        j.key("latestChapter").value(b.latestChapter);
        j.key("status").value(b.status);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleSoNovelDownload(const Request& req) {
    SoNovel::Book b;
    if (req.method == "POST") {
        b.sourceId = std::atoi(bodyString(req.body, "sourceId").c_str());
        b.url = bodyString(req.body, "url");
        b.bookName = bodyString(req.body, "name");
        b.author = bodyString(req.body, "author");
    } else {
        b.sourceId = queryInt(req, "sourceId", 0);
        b.url = queryStr(req, "url");
        b.bookName = queryStr(req, "name");
        b.author = queryStr(req, "author");
    }
    if (b.url.empty() || b.bookName.empty()) return badRequest("缺少参数 url / name");

    SoNovel svc;
    if (!svc.isAvailable()) {
        std::string home;
        if (ensureConfig()) home = state().cfg.soNovelHome();
        if (!svc.ensureService(home, 25)) {
            return Response::error("无法启动 so-novel: " + svc.lastError(), 503);
        }
    }

    std::string fileName = svc.download(b, "txt");
    if (fileName.empty()) return Response::error("下载失败: " + svc.lastError(), 500);

    std::string dir = downloadDir();
    if (dir.empty()) return Response::error("无法确定下载目录", 500);

    std::string localPath;
    if (!svc.fetchToLocal(fileName, dir, localPath)) {
        return Response::error("复制到本地失败: " + svc.lastError(), 500);
    }

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("serverFile").value(fileName);
    j.key("path").value(localPath);
    j.endObject();
    return okJson(j);
}

#endif // HAS_CURL

// 统一搜索：本地书库按文件名匹配，online=1 时附带番茄 / so-novel 在线书源结果。
// 在线结果直接嵌入各书源接口的原始 JSON，前端可以按书源分别渲染。
Response handleSearch(const Request& req) {
    std::string q = queryStr(req, "q");
    if (q.empty()) return badRequest("缺少参数 q");

    std::string needle = q;
    for (char& c : needle) c = (char)std::tolower((unsigned char)c);

    std::vector<std::string> files;
    std::string err;
    int status = 500;
    if (!gatherLibraryFiles(queryStr(req, "dir"), queryInt(req, "depth", 4),
                            files, err, status)) {
        return Response::error(err, status);
    }

    std::vector<std::string> hits;
    for (const auto& f : files) {
        std::string name = fsutil::filename(f);
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        if (name.find(needle) != std::string::npos) hits.push_back(f);
    }
    const bool truncated = hits.size() > 200;
    if (truncated) hits.resize(200);

    bool wantOnline = queryStr(req, "online") == "1";
    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("q").value(q);
    j.key("count").value((int)hits.size());
    j.key("truncated").value(truncated);
    j.key("local");
    j.beginArray();
    for (const auto& f : hits) writeLibraryBook(j, f);
    j.endArray();
    j.key("online").value(wantOnline);
#ifdef HAS_CURL
    j.key("hasOnline").value(true);
    if (wantOnline) {
        Request sub;
        sub.method = "GET";
        sub.query["q"] = q;
        sub.path = "/api/fanqie/search";
        std::string fb = handleFanqieSearch(sub).body;
        j.key("fanqie");
        j.raw(fb.empty() ? "{}" : fb);
        sub.path = "/api/sonovel/search";
        std::string sb = handleSoNovelSearch(sub).body;
        j.key("sonovel");
        j.raw(sb.empty() ? "{}" : sb);
    }
#else
    j.key("hasOnline").value(false);
    if (wantOnline) j.key("hint").value("在线书源需要 -DENABLE_CURL=ON 重新编译");
#endif
    j.endObject();
    return okJson(j);
}

#ifdef HAS_MUSIC

Response handleMusicLibrary(const Request& req) {
    std::string dir = queryStr(req, "dir");
    if (dir.empty()) {
        std::string base = fsutil::exeDir();
        dir = fsutil::join(fsutil::join(base, "downloads"), "music");
    }
    if (!fsutil::isDirectory(dir)) {
        // 目录尚未创建（还没下载过音乐）属于正常情况，返回空列表
        Json j;
        j.beginObject();
        j.key("dir").value(dir);
        j.key("count").value(0);
        j.key("songs");
        j.beginArray();
        j.endArray();
        j.key("note").value("目录不存在，尚未下载过音乐");
        j.endObject();
        return okJson(j);
    }

    auto songs = musiclib::scanDir(dir);
    Json j;
    j.beginObject();
    j.key("dir").value(dir);
    j.key("count").value((int)songs.size());
    j.key("songs");
    j.beginArray();
    for (const auto& s : songs) {
        j.beginObject();
        j.key("path").value(s.path);
        j.key("title").value(s.title);
        j.key("artist").value(s.artist);
        j.key("album").value(s.album);
        j.key("duration").value(s.durationSec);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleOnlineSearch(const Request& req) {
    std::string q = queryStr(req, "q");
    if (q.empty()) return badRequest("缺少参数 q");
    std::string source = queryStr(req, "source");

    OnlineMusicService svc;
    std::vector<std::string> sources;
    if (!source.empty()) sources.push_back(source);

    auto songs = svc.search(q, sources);

    Json j;
    j.beginObject();
    j.key("count").value((int)songs.size());
    if (songs.empty() && !svc.lastError().empty()) {
        j.key("error").value(svc.lastError());
    }
    j.key("songs");
    j.beginArray();
    for (const auto& s : songs) {
        j.beginObject();
        j.key("id").value(s.id);
        j.key("title").value(s.title);
        j.key("artist").value(s.artist);
        j.key("album").value(s.album);
        j.key("duration").value(s.durationSec);
        j.key("source").value(s.source);
        j.key("playable").value(!s.streamUrl.empty());
        j.key("hasLyric").value(!s.lyricText.empty());
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleOnlineSources(const Request&) {
    Json j;
    j.beginObject();
    j.key("sources");
    j.beginArray();
    for (const auto& s : OnlineMusicService::availableSources()) j.value(s);
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleOnlineResolve(const Request& req) {
    std::string id = queryStr(req, "id");
    std::string source = queryStr(req, "source", "netease");
    std::string title = queryStr(req, "title");
    if (id.empty()) return badRequest("缺少参数 id");

    OnlineSong s;
    s.id = id;
    s.source = source;
    s.title = title;

    OnlineMusicService svc;
    std::string url = svc.resolveStream(s);

    Json j;
    j.beginObject();
    j.key("ok").value(!url.empty());
    j.key("id").value(id);
    j.key("source").value(source);
    j.key("url").value(url);
    if (url.empty()) j.key("error").value(svc.lastError());
    j.endObject();
    return okJson(j);
}

// ==================== 网易云：登录 / 歌单 / 排行榜 ====================

Response handleNeteaseAccount(const Request&) {
    OnlineMusicService svc;
    NeteaseAccount acc;
    if (!svc.account(acc)) return Response::error("查询登录状态失败: " + svc.lastError(), 500);

    Json j;
    j.beginObject();
    j.key("loggedIn").value(acc.loggedIn);
    j.key("userId").value(acc.userId);
    j.key("nickname").value(acc.nickname);
    j.key("vip").value(acc.vip);
    j.key("sessionPath").value(OnlineMusicService::sessionPath());
    j.key("sessionPersisted").value(OnlineMusicService::sessionPersisted());
    j.endObject();
    return okJson(j);
}

// 手动导入 Cookie（扫码之外的第二条路径，也用于从浏览器迁移登录态）
// 请求体：{"cookie": "MUSIC_U=...; __csrf=...; ..."}
Response handleNeteaseCookieImport(const Request& req) {
    if (req.body.empty()) return badRequest("请求体为空，需要 {\"cookie\": \"...\"}");

    std::string raw = minijson::parse(req.body)["cookie"].asString();
    if (raw.empty()) raw = bodyString(req.body, "cookie");  // 兼容非标准 JSON
    if (raw.empty()) return badRequest("缺少 cookie 字段");

    OnlineMusicService svc;
    if (!svc.importNeteaseCookie(raw)) {
        return Response::error("导入失败: " + svc.lastError(), 400);
    }

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("persisted").value(true);
    j.key("sessionPath").value(OnlineMusicService::sessionPath());
    j.endObject();
    return okJson(j);
}

// 把登录链接编码成内联 SVG（终端版用 term_qr.h 的另一套渲染）。
std::string qrToSvg(const std::string& text, int border = 2, int scale = 6) {
    if (text.empty()) return "";
    try {
        qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
            text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        int n = qr.getSize();
        int dim = (n + border * 2) * scale;
        std::string s = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
            std::to_string(dim) + "\" height=\"" + std::to_string(dim) +
            "\" viewBox=\"0 0 " + std::to_string(dim) + " " + std::to_string(dim) + "\">" +
            "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/><g fill=\"#000\">";
        // 把每行连续的黑色模块合并成一个 rect，避免生成过大的 SVG
        for (int y = 0; y < n; y++) {
            int run = -1;
            for (int x = 0; x <= n; x++) {
                bool black = x < n && qr.getModule(x, y);
                if (black && run < 0) run = x;
                if (!black && run >= 0) {
                    s += "<rect x=\"" + std::to_string((run + border) * scale) +
                         "\" y=\"" + std::to_string((y + border) * scale) +
                         "\" width=\"" + std::to_string((x - run) * scale) +
                         "\" height=\"" + std::to_string(scale) + "\"/>";
                    run = -1;
                }
            }
        }
        s += "</g></svg>";
        return s;
    } catch (...) {
        return "";
    }
}

Response handleNeteaseQrStart(const Request&) {
    OnlineMusicService svc;
    std::string key, url, msg;
    if (!svc.beginQrLogin(key, url, msg)) {
        return Response::error("生成二维码失败: " + msg, 500);
    }

    Json j;
    j.beginObject();
    j.key("key").value(key);
    j.key("url").value(url);
    j.key("svg").value(qrToSvg(url));
    j.key("message").value(msg);
    j.endObject();
    return okJson(j);
}

Response handleNeteaseQrPoll(const Request& req) {
    std::string key = queryStr(req, "key");
    if (key.empty()) return badRequest("缺少参数 key");

    OnlineMusicService svc;
    int code = 801;
    std::string msg;
    if (!svc.pollQrLogin(key, code, msg)) {
        return Response::error("查询扫码状态失败: " + msg, 500);
    }

    Json j;
    j.beginObject();
    j.key("code").value(code);
    j.key("message").value(msg);
    j.key("loggedIn").value(code == 803);
    j.endObject();
    return okJson(j);
}

Response handleNeteaseLogout(const Request&) {
    OnlineMusicService svc;
    svc.logoutNetease();

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.endObject();
    return okJson(j);
}

namespace {
Response playlistsToJson(const std::vector<OnlinePlaylist>& list) {
    Json j;
    j.beginObject();
    j.key("count").value((int)list.size());
    j.key("playlists");
    j.beginArray();
    for (const auto& p : list) {
        j.beginObject();
        j.key("id").value(p.id);
        j.key("name").value(p.name);
        j.key("trackCount").value(p.trackCount);
        j.key("cover").value(p.coverUrl);
        j.key("creator").value(p.creator);
        j.key("toplist").value(p.isToplist);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response songsToJson(const std::vector<OnlineSong>& list) {
    Json j;
    j.beginObject();
    j.key("count").value((int)list.size());
    j.key("songs");
    j.beginArray();
    for (const auto& s : list) {
        j.beginObject();
        j.key("id").value(s.id);
        j.key("title").value(s.title);
        j.key("artist").value(s.artist);
        j.key("album").value(s.album);
        j.key("duration").value(s.durationSec);
        j.key("playable").value(!s.streamUrl.empty());
        j.key("hasLyric").value(!s.lyricText.empty());
        j.key("hasTranslation").value(!s.transLyric.empty());
        j.endObject();
    }
    j.endArray();
    j.endObject();
    return okJson(j);
}
} // namespace

Response handleNeteasePlaylists(const Request&) {
    OnlineMusicService svc;
    std::vector<OnlinePlaylist> list;
    if (!svc.neteasePlaylists(list)) {
        return Response::error("获取歌单失败: " + svc.lastError(), 500);
    }
    return playlistsToJson(list);
}

Response handleNeteasePlaylistSongs(const Request& req) {
    std::string id = queryStr(req, "id");
    if (id.empty()) return badRequest("缺少参数 id");

    OnlineMusicService svc;
    std::vector<OnlineSong> songs;
    if (!svc.neteasePlaylistSongs(id, songs)) {
        return Response::error("获取歌单歌曲失败: " + svc.lastError(), 500);
    }
    return songsToJson(songs);
}

Response handleNeteaseToplists(const Request&) {
    OnlineMusicService svc;
    std::vector<OnlinePlaylist> list;
    if (!svc.neteaseToplists(list)) {
        return Response::error("获取排行榜失败: " + svc.lastError(), 500);
    }
    return playlistsToJson(list);
}

Response handleNeteaseRecommend(const Request&) {
    OnlineMusicService svc;
    std::vector<OnlineSong> songs;
    if (!svc.neteaseRecommend(songs)) {
        return Response::error("获取每日推荐失败: " + svc.lastError(), 500);
    }
    return songsToJson(songs);
}

Response handleNeteaseLyric(const Request& req) {
    std::string id = queryStr(req, "id");
    if (id.empty()) return badRequest("缺少参数 id");

    OnlineMusicService svc;
    std::string lrc, trans;
    bool ok = svc.neteaseLyricEx(id, lrc, trans);

    Json j;
    j.beginObject();
    j.key("ok").value(ok);
    j.key("id").value(id);
    j.key("lrc").value(lrc);
    j.key("translated").value(trans);
    if (!ok) j.key("error").value(svc.lastError());
    j.endObject();
    return okJson(j);
}

Response handleNeteaseDownload(const Request& req) {
    // POST 支持 JSON 请求体（GUI），也兼容原来的查询串写法。
    auto field = [&](const std::string& key, const std::string& def) {
        if (req.method == "POST") {
            std::string v = bodyString(req.body, key);
            if (!v.empty()) return v;
        }
        return queryStr(req, key, def);
    };
    std::string id = field("id", "");
    std::string source = field("source", "netease");
    std::string title = field("title", "");
    std::string artist = field("artist", "");
    if (id.empty()) return badRequest("缺少参数 id");

    OnlineSong s;
    s.id = id;
    s.source = source;
    s.title = title;
    s.artist = artist;

    OnlineMusicService svc;
    std::string err;
    std::string path = svc.downloadSong(s, "", err);

    Json j;
    j.beginObject();
    j.key("ok").value(!path.empty());
    j.key("path").value(path);
    if (!path.empty() && !err.empty()) j.key("warning").value(err);
    if (path.empty()) j.key("error").value(err.empty() ? svc.lastError() : err);
    j.endObject();
    return okJson(j);
}

#endif // HAS_MUSIC

Response handleConfigGet(const Request&) {
    if (!ensureConfig()) return Response::error("配置加载失败", 500);
    const Config& cfg = state().cfg;

    Json j;
    j.beginObject();
    j.key("path").value(cfg.path());
    j.key("sortMode").value(cfg.sortMode());
    j.key("autoUpdateBooks").value(cfg.autoUpdateBooks());
    j.key("autoUpdateDownloader").value(cfg.autoUpdateDownloader());
    j.key("autoStartSoNovel").value(cfg.autoStartSoNovel());
    j.key("soNovelHome").value(cfg.soNovelHome());

    j.key("dirs");
    j.beginArray();
    for (const auto& d : cfg.dirs()) j.value(d);
    j.endArray();

    j.key("recent");
    j.beginArray();
    for (const auto& r : cfg.recent()) {
        j.beginObject();
        j.key("path").value(r.path);
        j.key("openedAt").value((long long)r.openedAt);
        j.endObject();
    }
    j.endArray();

    j.endObject();
    return okJson(j);
}

Response handleConfigSet(const Request& req) {
    if (!ensureConfig()) return Response::error("配置加载失败", 500);
    if (req.body.empty()) return badRequest("请求体为空，需要 JSON");

    Config& cfg = state().cfg;
    std::vector<std::string> changed;

    auto setBool = [&](const char* key, bool (Config::*getter)() const,
                       void (Config::*setter)(bool)) {
        if (req.body.find(std::string("\"") + key + "\"") == std::string::npos) return;
        bool v = bodyString(req.body, key) == "true" ||
                 req.body.find(std::string("\"") + key + "\":true") != std::string::npos;
        if ((cfg.*getter)() != v) {
            (cfg.*setter)(v);
            changed.push_back(key);
        }
    };

    setBool("autoUpdateBooks", &Config::autoUpdateBooks, &Config::setAutoUpdateBooks);
    setBool("autoUpdateDownloader", &Config::autoUpdateDownloader, &Config::setAutoUpdateDownloader);
    setBool("autoStartSoNovel", &Config::autoStartSoNovel, &Config::setAutoStartSoNovel);

    if (req.body.find("\"sortMode\"") != std::string::npos) {
        int m = queryInt(req, "sortMode", -1);
        if (m < 0) {
            std::string raw = bodyString(req.body, "sortMode");
            if (!raw.empty()) { try { m = std::stoi(raw); } catch (...) { m = -1; } }
        }
        if (m >= 0 && m <= 2 && m != cfg.sortMode()) {
            cfg.setSortMode(m);
            changed.push_back("sortMode");
        }
    }

    if (req.body.find("\"soNovelHome\"") != std::string::npos) {
        std::string home = bodyString(req.body, "soNovelHome");
        if (home != cfg.soNovelHome()) {
            if (!home.empty() && !fsutil::isDirectory(home)) {
                return badRequest("目录不存在: " + home);
            }
            cfg.setSoNovelHome(home);
            changed.push_back("soNovelHome");
        }
    }

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("changed");
    j.beginArray();
    for (const auto& c : changed) j.value(c);
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleConfigDirAdd(const Request& req) {
    if (!ensureConfig()) return Response::error("配置加载失败", 500);
    std::string dir = req.method == "POST" ? bodyString(req.body, "dir") : queryStr(req, "dir");
    if (dir.empty()) return badRequest("缺少参数 dir");
    if (!fsutil::isDirectory(dir)) return badRequest("目录不存在: " + dir);

    Config& cfg = state().cfg;
    if (cfg.hasLibraryDir(dir)) {
        Json j;
        j.beginObject();
        j.key("ok").value(true);
        j.key("added").value(false);
        j.endObject();
        return okJson(j);
    }
    cfg.addLibraryDir(dir);

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("added").value(true);
    j.key("dir").value(dir);
    j.endObject();
    return okJson(j);
}

Response handleConfigDirRemove(const Request& req) {
    if (!ensureConfig()) return Response::error("配置加载失败", 500);
    std::string dir = req.method == "POST" ? bodyString(req.body, "dir") : queryStr(req, "dir");
    if (dir.empty()) return badRequest("缺少参数 dir");

    Config& cfg = state().cfg;
    bool removed = cfg.removeLibraryDir(dir);

    Json j;
    j.beginObject();
    j.key("ok").value(true);
    j.key("removed").value(removed);
    j.endObject();
    return okJson(j);
}

} // namespace

void registerRoutes(Server& server) {
    // 基础
    server.route("GET", "/api/health", handleHealth);

    // 书库
    server.route("GET", "/api/library", handleLibrary);
    server.route("GET", "/api/library/detail", handleLibraryDetail);
    server.route("GET", "/api/search", handleSearch);

    // 阅读
    server.route("POST", "/api/book/open", handleBookOpen);
    server.route("GET", "/api/book/open", handleBookOpen);
    server.route("GET", "/api/book/info", handleBookInfo);
    server.route("GET", "/api/book/toc", handleBookToc);
    server.route("GET", "/api/book/chapter", handleBookChapter);
    server.route("GET", "/api/book/content", handleBookContent);
    server.route("GET", "/api/book/search", handleBookSearch);
    server.route("GET",  "/api/book/progress",  handleBookProgress);
    server.route("POST", "/api/book/progress",  handleBookProgress);
    server.route("GET",  "/api/book/bookmarks", handleBookBookmarks);
    server.route("POST", "/api/book/bookmarks", handleBookBookmarks);

#ifdef HAS_CURL
    // 番茄
    server.route("GET", "/api/fanqie/search", handleFanqieSearch);
    server.route("POST", "/api/fanqie/download", handleFanqieDownload);
    server.route("GET", "/api/fanqie/status", handleFanqieStatus);
    server.route("GET", "/api/fanqie/config", handleFanqieConfigGet);
    server.route("POST", "/api/fanqie/config", handleFanqieConfigSet);
    server.route("GET", "/api/fanqie/preview", handleFanqiePreview);
    server.route("GET", "/api/fanqie/cover", handleFanqieCover);
    server.route("POST", "/api/fanqie/job", handleFanqieJobStart);
    server.route("GET", "/api/fanqie/job", handleFanqieJob);
    server.route("POST", "/api/fanqie/job/action", handleFanqieJobAction);
    server.route("GET", "/api/fanqie/library", handleFanqieLibrary);
    server.route("POST", "/api/fanqie/import", handleFanqieImport);
    server.route("GET", "/api/fanqie/updates", handleFanqieUpdates);
    server.route("GET", "/api/fanqie/history", handleFanqieHistory);

    // so-novel
    server.route("GET", "/api/sonovel/search", handleSoNovelSearch);
    server.route("POST", "/api/sonovel/download", handleSoNovelDownload);
#endif

#if defined(HAS_GUI) || defined(HAS_MUSIC)
    // 本地曲库与音频文件（GUI 未编译音乐模块时仍可播放本地音频）
    server.route("GET", "/api/music/library", handleMusicLibrary);
    server.route("GET", "/api/music/file", handleMusicFile);
#endif

#ifdef HAS_MUSIC
    // 在线音乐
    server.route("GET", "/api/online/sources", handleOnlineSources);
    server.route("GET", "/api/online/search", handleOnlineSearch);
    server.route("GET", "/api/online/resolve", handleOnlineResolve);
    server.route("POST", "/api/online/download", handleNeteaseDownload);

    // 网易云账号
    server.route("GET", "/api/netease/account", handleNeteaseAccount);
    server.route("POST", "/api/netease/login/qr", handleNeteaseQrStart);
    server.route("GET", "/api/netease/login/poll", handleNeteaseQrPoll);
    server.route("POST", "/api/netease/login/cookie", handleNeteaseCookieImport);
    server.route("POST", "/api/netease/logout", handleNeteaseLogout);

    // 歌单 / 排行榜 / 推荐 / 歌词
    server.route("GET", "/api/netease/playlists", handleNeteasePlaylists);
    server.route("GET", "/api/netease/playlist/songs", handleNeteasePlaylistSongs);
    server.route("GET", "/api/netease/toplists", handleNeteaseToplists);
    server.route("GET", "/api/netease/recommend", handleNeteaseRecommend);
    server.route("GET", "/api/netease/lyric", handleNeteaseLyric);
#endif

    // 配置
    server.route("GET", "/api/config", handleConfigGet);
    server.route("POST", "/api/config", handleConfigSet);
    server.route("POST", "/api/config/dir/add", handleConfigDirAdd);
    server.route("POST", "/api/config/dir/remove", handleConfigDirRemove);

#ifdef HAS_GUI
    // 编译进 GUI 时，"--serve" 也会顺带提供阅读器页面
    gui::registerRoutes(server);
#endif
}

} // namespace api
