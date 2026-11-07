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
#endif

#ifdef HAS_GUI
#include "gui.h"
#endif

#include <algorithm>
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
#ifdef HAS_CURL
    caps.push_back("fanqie");
    caps.push_back("sonovel");
#endif
#ifdef HAS_MUSIC
    caps.push_back("music");
    caps.push_back("online-music");
#endif
    j.key("capabilities");
    j.beginArray();
    for (const auto& c : caps) j.value(c);
    j.endArray();
    j.endObject();
    return okJson(j);
}

Response handleLibrary(const Request& req) {
    if (!ensureConfig()) return Response::error("配置加载失败", 500);

    std::string dir = queryStr(req, "dir");
    int maxDepth = queryInt(req, "depth", 4);

    std::vector<std::string> files;
    if (!dir.empty()) {
        if (!fsutil::isDirectory(dir)) return badRequest("目录不存在: " + dir);
        files = Config::scanDir(dir, maxDepth);
    } else {
        // 默认：downloads/ + 配置里登记的目录
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

    Json j;
    j.beginObject();
    j.key("count").value((int)files.size());
    j.key("books");
    j.beginArray();
    for (const auto& f : files) {
        j.beginObject();
        j.key("path").value(f);
        j.key("name").value(fsutil::filename(f));
        j.key("size").value((long long)fsutil::fileSize(f));
        j.key("mtime").value((long long)fsutil::lastWriteMs(f));
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
    std::string id = queryStr(req, "id");
    std::string source = queryStr(req, "source", "netease");
    std::string title = queryStr(req, "title");
    std::string artist = queryStr(req, "artist");
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

    // so-novel
    server.route("GET", "/api/sonovel/search", handleSoNovelSearch);
    server.route("POST", "/api/sonovel/download", handleSoNovelDownload);
#endif

#ifdef HAS_MUSIC
    // 本地曲库
    server.route("GET", "/api/music/library", handleMusicLibrary);

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
