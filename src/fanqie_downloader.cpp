#include "fanqie_downloader.h"
#include "http_client.h"
#include "mini_json.h"
#include "fanqie_resource.h"
#include "file_io.h"
#include "fs_util.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

namespace {

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool fileExists(const std::string& path) {
    return fsutil::isFile(path);
}

// 当前平台的子目录名（用于 tools/tomato/<platform>/ 布局）
std::string platformSubdir() {
#if defined(_WIN32)
    return "win64";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "macos_arm64";
#else
    return "macos_amd64";
#endif
#else
#if defined(__aarch64__) || defined(__arm64__)
    return "linux_arm64";
#else
    return "linux_amd64";
#endif
#endif
}

// 平台对应的可执行文件名
std::string platformExeName() {
#ifdef _WIN32
    return "TomatoNovelDownloader.exe";
#else
    return "TomatoNovelDownloader";
#endif
}
} // namespace

FanqieDownloader::FanqieDownloader(int concurrencyHint)
    : concurrency(concurrencyHint > 0 ? concurrencyHint : 4), serviceChecked(false) {
    const char* base = std::getenv("FANQIE_API_BASE");
    baseUrl = base ? std::string(base) : std::string("http://127.0.0.1:18423");
    while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();

    // 外置方案：直接定位已有的下载器，不做任何释放
    toolDir = fanqie_res::downloaderDir();
    if (toolDir.empty()) {
        // 找不到时给一个合理默认值，后续 spawnService 会失败并给出提示
        std::string base = fsutil::exeDir();
        toolDir = base.empty() ? "tools/tomato"
                               : fsutil::join(fsutil::join(base, "tools"), "tomato");
    }

    // 下载目录：exe 同级的 downloads/
    std::string exeBase = fsutil::exeDir();
    if (exeBase.empty()) exeBase = ".";
    defaultSaveDir = fsutil::join(exeBase, "downloads");
    fsutil::createDirs(defaultSaveDir);
}

FanqieDownloader::~FanqieDownloader() {}

std::string FanqieDownloader::urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::string FanqieDownloader::sanitizeName(const std::string& name) {
    std::string out = name;
    const std::string illegal = "\\/:*?\"<>|";
    for (char c : illegal) {
        std::replace(out.begin(), out.end(), c, '_');
    }
    if (out.empty()) out = "untitled";
    return out;
}

std::string FanqieDownloader::httpGet(const std::string& path, const std::string& query) {
    HttpClient client;
    client.setTimeout(60);
    std::string url = baseUrl + path;
    if (!query.empty()) url += "?" + query;
    return client.get(url);
}

std::string FanqieDownloader::httpPostJson(const std::string& path, const std::string& jsonBody) {
    HttpClient client;
    client.setTimeout(60);
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    return client.post(baseUrl + path, jsonBody, headers);
}

bool FanqieDownloader::isServiceUp() {
    HttpClient client;
    client.setTimeout(3);
    std::string r = client.get(baseUrl + "/api/config");
    return !r.empty() && r.find("\"novel_format\"") != std::string::npos;
}

std::string FanqieDownloader::locateExe() {
    std::vector<std::string> candidates;

    // 优先平台子目录（随包分发的多平台布局）
    candidates.push_back(fsutil::join(fsutil::join(toolDir, platformSubdir()), platformExeName()));
    // 其次工具目录根
    candidates.push_back(fsutil::join(toolDir, platformExeName()));
#ifndef _WIN32
    candidates.push_back(fsutil::join(toolDir, "tomato-novel-downloader"));
#endif
    for (const auto& c : candidates) {
        if (fileExists(c)) return c;
    }
    return "";
}

bool FanqieDownloader::spawnService() {
    std::string exe = locateExe();

    if (exe.empty()) {
        // 尝试用资源模块重新定位一次（覆盖 exe 目录变化的情况）
        exe = fanqie_res::locateDownloader();
        if (!exe.empty()) {
            toolDir = fanqie_res::downloaderDir();
        }
    }
    if (exe.empty()) {
        std::cerr << "[fanqie] " << fanqie_res::missingHint() << std::endl;
        return false;
    }

#ifdef _WIN32
    std::string cmd = "\"" + exe + "\" --server";
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string workDir = toolDir;
    BOOL ok = CreateProcessA(
        exe.c_str(), cmdBuf.data(), NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, workDir.c_str(), &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    // 确保有执行权限（内嵌释放后文件默认 0644）
    ::chmod(exe.c_str(), 0755);

    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        if (::chdir(toolDir.c_str()) != 0) { /* 失败也继续 */ }
        int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > 2) ::close(devNull);
        }
        ::execl(exe.c_str(), exe.c_str(), "--server", (char*)NULL);
        ::_exit(127);
    }
    return true;
#endif
}

bool FanqieDownloader::ensureService() {
    if (isServiceUp()) {
        serviceChecked = true;
        return true;
    }
    if (!spawnService()) {
        std::cerr << "[fanqie] 无法启动服务。toolDir=" << toolDir
                  << "，exe=" << (locateExe().empty() ? "(未找到)" : locateExe()) << std::endl;
        return false;
    }

    for (int i = 0; i < 40; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (isServiceUp()) {
            serviceChecked = true;
            return true;
        }
    }
    return false;
}

std::vector<FanqieDownloader::BookInfo> FanqieDownloader::searchBooks(const std::string& keyword) {
    std::vector<BookInfo> books;
    if (!ensureService()) return books;

    std::string body = httpGet("/api/search", "q=" + urlEncode(keyword));
    if (body.empty()) return books;

    minijson::Value root = minijson::parse(body);
    const minijson::Value& items = root["items"];
    for (size_t i = 0; i < items.size(); i++) {
        const minijson::Value& it = items[i];
        BookInfo b;
        b.book_id = it["book_id"].toStringValue();
        if (b.book_id.empty()) continue;
        b.book_name = it["book_name"].toStringValue();
        if (b.book_name.empty()) b.book_name = it["title"].toStringValue();
        if (b.book_name.empty()) b.book_name = "未知";
        b.author = it["author"].toStringValue();
        if (b.author.empty()) b.author = "未知";

        const minijson::Value& raw = it["raw"];
        std::string score = it["score"].toStringValue();
        if (score.empty()) score = raw["score"].toStringValue();
        b.score = score;

        std::string abs = it["abstract"].toStringValue();
        if (abs.empty()) abs = raw["abstract"].toStringValue();
        abs = replaceAll(abs, "\n", " ");
        abs = replaceAll(abs, "\r", " ");
        if (abs.size() > 210) {
            size_t cut = 210;
            while (cut > 0 && ((unsigned char)abs[cut] & 0xC0) == 0x80) cut--;
            abs = abs.substr(0, cut);
        }
        b.abstract = abs;

        books.push_back(b);
    }
    return books;
}

std::string FanqieDownloader::readChaptersJsonl(const std::string& bookId, int maxChapters) {
    std::string p = fsutil::join(fsutil::join(toolDir, sanitizeName(bookId)), "downloaded_chapters.jsonl");
    std::string data;
    if (!fsutil::readAll(p, data)) return "";
    std::istringstream in(data);

    std::string out;
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        minijson::Value v = minijson::parse(line);
        std::string title = v["title"].toStringValue();
        std::string content = v["content"].toStringValue();
        if (content.empty()) continue;

        std::string body = stripHtml(content);
        out += "\n\n";
        // 正文首行常已包含标题，避免重复
        if (!title.empty()) {
            std::string head = body.substr(0, std::min(body.size(), title.size() + 8));
            if (head.find(title) == std::string::npos) {
                out += title + "\n\n";
            }
        }
        out += body;
        count++;
        if (maxChapters > 0 && count >= maxChapters) break;
    }
    return out;
}

std::string FanqieDownloader::stripHtml(const std::string& html) {
    std::string s = html;
    s = replaceAll(s, "</p>", "\n");
    s = replaceAll(s, "</h1>", "\n");
    s = replaceAll(s, "</h2>", "\n");
    s = replaceAll(s, "<br/>", "\n");
    s = replaceAll(s, "<br />", "\n");
    s = replaceAll(s, "<br>", "\n");

    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) out += c;
    }

    out = replaceAll(out, "&nbsp;", " ");
    out = replaceAll(out, "&amp;", "&");
    out = replaceAll(out, "&lt;", "<");
    out = replaceAll(out, "&gt;", ">");
    out = replaceAll(out, "&quot;", "\"");

    // 折叠过多空行
    std::string cleaned;
    cleaned.reserve(out.size());
    int nlRun = 0;
    for (char c : out) {
        if (c == '\n') {
            nlRun++;
            if (nlRun <= 2) cleaned += c;
        } else {
            nlRun = 0;
            cleaned += c;
        }
    }
    return trimStr(cleaned);
}

bool FanqieDownloader::downloadBook(const std::string& bookId, const std::string& bookName,
                                    const std::string& saveDir, int maxChapters) {
    std::string body = buildBookText(bookId, maxChapters);
    if (body.empty()) return false;

    // 元信息头（取不到就退化为纯正文）
    BookMeta meta = fetchBookMeta(bookId);
    if (!meta.valid) {
        meta.name = bookName;
        meta.valid = !bookName.empty();
    }

    std::string text = formatMetaHeader(meta) + body;

    std::string outDir = saveDir.empty() ? defaultSaveDir : saveDir;
    fsutil::createDirs(outDir);

    std::string useName = meta.valid && !meta.name.empty() ? meta.name : bookName;
    std::string outPath = fsutil::join(outDir, sanitizeName(useName) + ".txt");
    return fsutil::writeAll(outPath, text);
}

std::string FanqieDownloader::buildBookText(const std::string& bookId, int maxChapters) {
    if (!ensureService()) return "";

    std::ostringstream body;
    body << "{\"book_id\":\"" << bookId << "\"";
    if (maxChapters > 0) {
        body << ",\"range_start\":1,\"range_end\":" << maxChapters;
    }
    body << "}";

    std::string resp = httpPostJson("/api/jobs", body.str());
    if (resp.empty()) return "";

    minijson::Value job = minijson::parse(resp);
    std::string jobId = job["id"].toStringValue();
    if (jobId.empty()) return "";

    // 轮询任务状态
    std::string state;
    for (int i = 0; i < 600; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::string s = httpGet("/api/jobs", "id=" + jobId);
        if (s.empty()) continue;
        minijson::Value sr = minijson::parse(s);
        const minijson::Value& items = sr["items"];
        if (items.size() == 0) continue;
        state = items[0]["state"].toStringValue();
        if (state == "done") break;
        if (state == "failed" || state == "cancelled") return "";
    }
    if (state != "done") return "";

    return readChaptersJsonl(bookId, maxChapters);
}

std::vector<FanqieDownloader::ChapterInfo> FanqieDownloader::getChapterList(const std::string& bookId) {
    std::vector<ChapterInfo> chapters;
    std::string p = fsutil::join(fsutil::join(toolDir, sanitizeName(bookId)), "downloaded_chapters.jsonl");
    std::string data;
    if (!fsutil::readAll(p, data)) return chapters;
    std::istringstream in(data);

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        minijson::Value v = minijson::parse(line);
        ChapterInfo c;
        c.item_id = v["id"].toStringValue();
        c.title = v["title"].toStringValue();
        if (!c.title.empty()) chapters.push_back(c);
    }
    return chapters;
}

FanqieDownloader::BookMeta FanqieDownloader::fetchBookMeta(const std::string& bookId) {
    BookMeta m;
    if (!ensureService()) return m;

    std::string body = httpGet("/api/preview/" + urlEncode(bookId), "");
    if (body.empty()) return m;

    minijson::Value root = minijson::parse(body);
    if (root.type != minijson::Value::Object) return m;

    m.name = root["book_name"].toStringValue();
    if (m.name.empty()) m.name = root["original_book_name"].toStringValue();
    m.author = root["author"].toStringValue();
    m.category = root["category"].toStringValue();
    m.description = root["description"].toStringValue();
    m.score = root["score"].toStringValue();
    m.chapterCount = (int)root["chapter_count"].asNumber();
    m.wordCount = (long long)root["word_count"].asNumber();
    m.finished = root["finished"].asBool();

    const minijson::Value& tags = root["tags"];
    for (size_t i = 0; i < tags.size(); i++) {
        std::string t = tags[i].toStringValue();
        if (t.empty()) continue;
        if (!m.tags.empty()) m.tags += "|";
        m.tags += t;
    }

    m.valid = !m.name.empty();
    return m;
}

std::string FanqieDownloader::formatMetaHeader(const BookMeta& meta) {
    if (!meta.valid) return "";

    std::string out;
    out += meta.name;
    out += "\n";
    if (!meta.author.empty()) out += "作者：" + meta.author + "\n";

    std::string line2;
    if (!meta.category.empty()) line2 += "分类：" + meta.category + "　";
    line2 += "状态：" + std::string(meta.finished ? "完结" : "连载") + "　";
    if (!meta.score.empty()) {
        std::string sc = meta.score;
        size_t dot = sc.find('.');
        if (dot != std::string::npos && sc.size() > dot + 2) sc = sc.substr(0, dot + 2);
        line2 += "评分：" + sc + "　";
    }
    if (meta.wordCount > 0) {
        char buf[64];
        if (meta.wordCount >= 10000) {
            std::snprintf(buf, sizeof(buf), "字数：%.1f万", (double)meta.wordCount / 10000.0);
        } else {
            std::snprintf(buf, sizeof(buf), "字数：%lld", meta.wordCount);
        }
        line2 += buf;
    }
    if (!line2.empty()) out += line2 + "\n";

    if (!meta.tags.empty()) out += "标签：" + meta.tags + "\n";
    if (meta.chapterCount > 0) out += "章节：" + std::to_string(meta.chapterCount) + "\n";

    if (!meta.description.empty()) {
        out += "\n【简介】\n";
        std::string desc = replaceAll(meta.description, "\r\n", "\n");
        desc = replaceAll(desc, "\r", "\n");
        out += trimStr(desc);
        out += "\n";
    }

    out += "\n";
    out += std::string(60, '=');
    out += "\n";
    return out;
}

// ==================== 增量更新 ====================

int FanqieDownloader::updateBook(const std::string& bookId, const std::string& bookName,
                                  const std::string& saveDir) {
    if (!ensureService()) return -1;

    // 记录更新前的章节数
    std::string jsonlPath = fsutil::join(fsutil::join(toolDir, sanitizeName(bookId)),
                                          "downloaded_chapters.jsonl");
    int before = 0;
    {
        std::string data;
        if (fsutil::readAll(jsonlPath, data)) {
            std::istringstream in(data);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) before++;
            }
        }
    }

    // 触发一次下载任务（服务端会跳过已下载的章节）
    std::ostringstream body;
    body << "{\"book_id\":\"" << bookId << "\"}";

    std::string resp = httpPostJson("/api/jobs", body.str());
    if (resp.empty()) return -1;

    minijson::Value job = minijson::parse(resp);
    std::string jobId = job["id"].toStringValue();
    if (jobId.empty()) return -1;

    std::string state;
    for (int i = 0; i < 600; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::string s = httpGet("/api/jobs", "id=" + jobId);
        if (s.empty()) continue;
        minijson::Value sr = minijson::parse(s);
        const minijson::Value& items = sr["items"];
        if (items.size() == 0) continue;
        state = items[0]["state"].toStringValue();
        if (state == "done") break;
        if (state == "failed" || state == "cancelled") return -1;
    }
    if (state != "done") return -1;

    // 统计更新后的章节数
    int after = before;
    {
        std::string data;
        if (fsutil::readAll(jsonlPath, data)) {
            after = 0;
            std::istringstream in(data);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) after++;
            }
        }
    }

    int added = after - before;

    // 有新增章节时重新导出 txt
    if (added > 0) {
        std::string body = readChaptersJsonl(bookId, -1);
        if (!body.empty()) {
            BookMeta meta = fetchBookMeta(bookId);
            if (!meta.valid) {
                meta.name = bookName;
                meta.valid = !bookName.empty();
            }
            std::string text = formatMetaHeader(meta) + body;

            std::string outDir = saveDir.empty() ? defaultSaveDir : saveDir;
            fsutil::createDirs(outDir);

            std::string useName = meta.valid && !meta.name.empty() ? meta.name : bookName;
            std::string outPath = fsutil::join(outDir, sanitizeName(useName) + ".txt");
            fsutil::writeAllAtomic(outPath, text);
        }
    }

    return added;
}

// ==================== 服务端能力（下载器 2.4.x） ====================

namespace {

std::string pctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// 按 / 分段编码，保留层级
std::string encodeRelPath(const std::string& rel) {
    std::string out;
    std::string seg;
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            out += pctEncode(seg);
            out += '/';
            seg.clear();
        } else {
            seg += c;
        }
    }
    out += pctEncode(seg);
    return out;
}

void collectChoices(const minijson::Value& v, std::vector<std::string>& out) {
    for (size_t i = 0; i < v.size(); i++) {
        const minijson::Value& e = v[i];
        std::string s;
        if (e.type == minijson::Value::Object) {
            const char* keys[] = {"value", "format", "name", "label", "title"};
            for (const char* k : keys) {
                s = e[k].toStringValue();
                if (!s.empty()) break;
            }
        } else {
            s = e.toStringValue();
        }
        if (!s.empty()) out.push_back(s);
    }
}

} // namespace

FanqieDownloader::ServerStatus FanqieDownloader::status() {
    ServerStatus st;
    if (!ensureService()) return st;

    std::string body = httpGet("/api/status", "");
    if (body.empty()) return st;

    minijson::Value r = minijson::parse(body);
    if (r.type != minijson::Value::Object) return st;

    st.ok = true;
    st.version = r["version"].toStringValue();
    st.saveDir = r["save_dir"].toStringValue();
    st.bindAddr = r["bind_addr"].toStringValue();
    st.locked = r["locked"].asBool();
    st.docker = r["docker_build"].asBool();
    return st;
}

FanqieDownloader::WebConfig FanqieDownloader::webConfig() {
    WebConfig c;
    if (!ensureService()) return c;

    std::string body = httpGet("/api/config", "");
    if (body.empty()) return c;

    minijson::Value r = minijson::parse(body);
    if (r.type != minijson::Value::Object) return c;

    c.valid = true;
    c.novelFormat = r["novel_format"].toStringValue();
    if (c.novelFormat.empty()) c.novelFormat = "txt";
    c.bulkFiles = r["bulk_files"].asBool();
    c.enableAudiobook = r["enable_audiobook"].asBool();
    c.audiobookFormat = r["audiobook_format"].toStringValue();
    if (c.audiobookFormat.empty()) c.audiobookFormat = "mp3";
    c.askFormat = r["ask_format_after_download"].asBool();
    return c;
}

bool FanqieDownloader::applyConfig(const WebConfig& cfg, std::string& err) {
    err.clear();
    if (!ensureService()) { err = "下载器服务不可用"; return false; }

    std::ostringstream body;
    body << "{";
    if (!cfg.novelFormat.empty()) body << "\"novel_format\":\"" << cfg.novelFormat << "\"";
    body << ",\"bulk_files\":" << (cfg.bulkFiles ? "true" : "false");
    body << ",\"enable_audiobook\":" << (cfg.enableAudiobook ? "true" : "false");
    if (!cfg.audiobookFormat.empty()) body << ",\"audiobook_format\":\"" << cfg.audiobookFormat << "\"";
    body << "}";

    std::string resp = httpPostJson("/api/config", body.str());
    if (resp.empty()) { err = "服务未接受配置（可能版本不支持）"; return false; }
    if (resp.find("\"error\"") != std::string::npos) {
        minijson::Value r = minijson::parse(resp);
        err = r["error"].toStringValue();
        if (err.empty()) err = resp.substr(0, 120);
        return false;
    }
    return true;
}

FanqieDownloader::PreviewInfo FanqieDownloader::preview(const std::string& bookId) {
    PreviewInfo p;
    if (bookId.empty() || !ensureService()) return p;

    std::string body = httpGet("/api/preview/" + urlEncode(bookId), "");
    if (body.empty()) return p;

    minijson::Value r = minijson::parse(body);
    if (r.type != minijson::Value::Object) return p;

    p.bookId = r["book_id"].toStringValue();
    if (p.bookId.empty()) p.bookId = bookId;
    p.name = r["book_name"].toStringValue();
    p.originalName = r["original_book_name"].toStringValue();
    p.author = r["author"].toStringValue();
    p.category = r["category"].toStringValue();
    p.description = r["description"].toStringValue();
    p.tags = r["tags"].toStringValue();
    p.score = r["score"].toStringValue();
    p.readCountText = r["read_count_text"].toStringValue();
    p.firstChapter = r["first_chapter_title"].toStringValue();
    p.lastChapter = r["last_chapter_title"].toStringValue();
    p.coverUrl = r["cover_url"].toStringValue();
    p.chapterCount = (int)r["chapter_count"].asNumber();
    p.wordCount = (long long)r["word_count"].asNumber();
    p.finished = r["finished"].asBool();
    p.valid = !p.name.empty() || !p.bookId.empty();
    return p;
}

int FanqieDownloader::startJob(const std::string& bookId, const DownloadOptions& opt,
                               std::string& err) {
    err.clear();
    if (bookId.empty()) { err = "缺少 bookId"; return 0; }
    if (!ensureService()) { err = "下载器服务不可用"; return 0; }

    // 需要换格式 / 开关有声书时，先写服务端配置（任务会按配置产出文件）
    if (!opt.format.empty() || opt.audiobookMode >= 0 || !opt.audiobookFormat.empty()) {
        WebConfig cfg = webConfig();
        if (cfg.valid) {
            bool dirty = false;
            if (!opt.format.empty() && cfg.novelFormat != opt.format) {
                cfg.novelFormat = opt.format;
                dirty = true;
            }
            if (opt.audiobookMode >= 0) {
                bool want = opt.audiobookMode == 1;
                if (cfg.enableAudiobook != want) { cfg.enableAudiobook = want; dirty = true; }
            }
            if (!opt.audiobookFormat.empty() && cfg.audiobookFormat != opt.audiobookFormat) {
                cfg.audiobookFormat = opt.audiobookFormat;
                dirty = true;
            }
            if (dirty) {
                std::string e;
                if (!applyConfig(cfg, e)) { err = e; return 0; }
            }
        }
    }

    std::ostringstream body;
    body << "{\"book_id\":\"" << bookId << "\"";
    if (opt.rangeStart > 0 && opt.rangeEnd >= opt.rangeStart) {
        body << ",\"range_start\":" << opt.rangeStart
             << ",\"range_end\":" << opt.rangeEnd;
    }
    body << "}";

    std::string resp = httpPostJson("/api/jobs", body.str());
    if (resp.empty()) { err = "创建任务失败（服务未响应）"; return 0; }

    minijson::Value r = minijson::parse(resp);
    int id = (int)r["id"].asNumber();
    if (id <= 0) {
        err = r["message"].toStringValue();
        if (err.empty()) err = "创建任务失败";
        return 0;
    }
    return id;
}

FanqieDownloader::JobInfo FanqieDownloader::job(int id) {
    JobInfo j;
    if (id <= 0 || !ensureService()) return j;

    std::string s = httpGet("/api/jobs", "id=" + std::to_string(id));
    if (s.empty()) return j;

    minijson::Value r = minijson::parse(s);
    const minijson::Value& items = r["items"];
    if (items.size() == 0) return j;

    const minijson::Value& it = items[0];
    j.valid = true;
    j.id = (int)it["id"].asNumber();
    j.bookId = it["book_id"].toStringValue();
    j.title = it["title"].toStringValue();
    j.author = it["author"].toStringValue();
    j.state = it["state"].toStringValue();
    j.message = it["message"].toStringValue();

    const minijson::Value& prog = it["progress"];
    j.savedChapters = (int)prog["saved_chapters"].asNumber();
    j.totalChapters = (int)prog["chapter_total"].asNumber();
    j.savePhase = prog["save_phase"].toStringValue();

    collectChoices(it["format_options"], j.formatOptions);
    collectChoices(it["book_name_options"], j.bookNameOptions);
    return j;
}

bool FanqieDownloader::cancelJob(int id) {
    if (id <= 0 || !ensureService()) return false;
    std::string r = httpPostJson("/api/jobs/" + std::to_string(id) + "/cancel", "{}");
    return !r.empty();
}

bool FanqieDownloader::chooseFormat(int id, const std::string& format) {
    if (id <= 0 || format.empty() || !ensureService()) return false;
    std::string r = httpPostJson("/api/jobs/" + std::to_string(id) + "/format",
                                 "{\"value\":\"" + format + "\"}");
    return !r.empty();
}

bool FanqieDownloader::chooseBookName(int id, const std::string& name) {
    if (id <= 0 || !ensureService()) return false;
    std::string body = name.empty() ? std::string("{\"value\":null}")
                                    : "{\"value\":\"" + name + "\"}";
    std::string r = httpPostJson("/api/jobs/" + std::to_string(id) + "/book_name", body);
    return !r.empty();
}

std::vector<FanqieDownloader::LibraryEntry> FanqieDownloader::library(
    const std::string& path, const std::string& name, bool startScan) {
    std::vector<LibraryEntry> out;
    if (!ensureService()) return out;

    std::string q = std::string("start=") + (startScan ? "true" : "false");
    if (!path.empty()) q += "&path=" + urlEncode(path);
    if (!name.empty()) q += "&name=" + urlEncode(name);

    std::string s = httpGet("/api/library", q);
    if (s.empty()) return out;

    minijson::Value r = minijson::parse(s);
    const minijson::Value& items = r["items"];
    for (size_t i = 0; i < items.size(); i++) {
        const minijson::Value& it = items[i];
        LibraryEntry e;
        e.kind = it["kind"].toStringValue();
        e.name = it["name"].toStringValue();
        e.relPath = it["rel_path"].toStringValue();
        e.ext = it["ext"].toStringValue();
        e.size = (long long)it["size"].asNumber();
        if (e.name.empty()) continue;
        out.push_back(e);
    }
    return out;
}

bool FanqieDownloader::fetchServerFile(const std::string& relPath, std::string& bytes) {
    bytes.clear();
    if (relPath.empty() || !ensureService()) return false;

    HttpClient client;
    client.setTimeout(180);
    bytes = client.get(baseUrl + "/download/" + encodeRelPath(relPath));
    return !bytes.empty();
}

bool FanqieDownloader::fetchCover(const std::string& bookId, std::string& bytes) {
    bytes.clear();
    if (bookId.empty() || !ensureService()) return false;

    HttpClient client;
    client.setTimeout(30);
    bytes = client.get(baseUrl + "/api/preview-cover-by-book/" + urlEncode(bookId));
    return !bytes.empty();
}

std::string FanqieDownloader::importProduced(const std::string& bookId,
                                             const std::string& bookName,
                                             const std::string& format,
                                             const std::string& localDir,
                                             std::string& err, int waitSeconds) {
    err.clear();
    std::string fmt = format.empty() ? "txt" : format;
    std::string outDir = localDir;
    fsutil::createDirs(outDir);

    auto save = [&](const std::string& relPath, const std::string& stem) -> std::string {
        std::string bytes;
        if (!fetchServerFile(relPath, bytes) || bytes.empty()) return "";
        std::string outPath = fsutil::join(outDir, sanitizeName(stem) + "." + fmt);
        if (!fsutil::writeAllAtomic(outPath, bytes)) { err = "写入本地文件失败"; return ""; }
        return outPath;
    };

    // 合并单文件模式下文件名是确定的（保存目录/书名.格式），直接按名取，避免整库扫描。
    std::vector<std::string> stems;
    if (!bookName.empty()) {
        stems.push_back(sanitizeName(bookName));
        if (sanitizeName(bookName) != bookName) stems.push_back(bookName);
    }

    // 书名可能被服务端清洗成别的形式，取一次预览补上原始书名
    auto addPreviewNames = [&]() {
        if (bookId.empty()) return;
        PreviewInfo p = preview(bookId);
        if (!p.valid) return;
        for (const std::string& n : {p.name, p.originalName}) {
            if (n.empty()) continue;
            std::string s = sanitizeName(n);
            if (std::find(stems.begin(), stems.end(), s) == stems.end()) stems.push_back(s);
            if (std::find(stems.begin(), stems.end(), n) == stems.end()) stems.push_back(n);
        }
    };

    for (int i = 0; i <= waitSeconds; i++) {
        for (const auto& s : stems) {
            std::string p = save(s + "." + fmt, s);
            if (!p.empty()) return p;
        }
        if (i == 2) addPreviewNames();

        // 兜底：列服务端书库按书名过滤（大目录扫描较慢，放在后面）
        if (i >= 2 && !bookName.empty()) {
            auto items = library("", bookName, /*startScan=*/i == 2);
            for (const auto& it : items) {
                if (it.kind != "file" || it.ext != fmt) continue;
                std::string p = save(it.relPath, fsutil::stem(it.name));
                if (!p.empty()) return p;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    err = "服务端没有找到导出的 " + fmt + " 文件（可能仍在校验或书源失败）";
    return "";
}

std::vector<FanqieDownloader::UpdateEntry> FanqieDownloader::updates(bool startScan,
                                                                    bool& running) {
    std::vector<UpdateEntry> out;
    running = false;
    if (!ensureService()) return out;

    std::string s = httpGet("/api/updates", startScan ? "start=true" : "start=false");
    if (s.empty()) return out;

    minijson::Value r = minijson::parse(s);
    running = r["running"].asBool();
    const minijson::Value& items = r["updates"];
    for (size_t i = 0; i < items.size(); i++) {
        const minijson::Value& it = items[i];
        UpdateEntry e;
        e.bookId = it["book_id"].toStringValue();
        e.bookName = it["book_name"].toStringValue();
        e.folder = it["folder"].toStringValue();
        e.localTotal = (int)it["local_total"].asNumber();
        e.remoteTotal = (int)it["remote_total"].asNumber();
        e.newCount = (int)it["new_count"].asNumber();
        e.hasUpdate = it["has_update"].asBool();
        if (e.bookName.empty()) continue;
        out.push_back(e);
    }
    return out;
}

std::vector<FanqieDownloader::HistoryEntry> FanqieDownloader::history(int limit) {
    std::vector<HistoryEntry> out;
    if (!ensureService()) return out;

    std::string s = httpGet("/api/history", "limit=" + std::to_string(limit));
    if (s.empty()) return out;

    minijson::Value r = minijson::parse(s);
    const minijson::Value& items = r["items"];
    for (size_t i = 0; i < items.size(); i++) {
        const minijson::Value& it = items[i];
        HistoryEntry e;
        e.timestamp = it["timestamp"].toStringValue();
        e.bookId = it["book_id"].toStringValue();
        e.bookName = it["book_name"].toStringValue();
        e.author = it["author"].toStringValue();
        e.status = it["status"].toStringValue();
        e.progressText = it["progress"].toStringValue();
        e.selectedChapters = (int)it["selected_chapters"].asNumber();
        e.successChapters = (int)it["success_chapters"].asNumber();
        e.failedChapters = (int)it["failed_chapters"].asNumber();
        if (e.bookName.empty()) continue;
        out.push_back(e);
    }
    return out;
}

// ==================== 下载器自更新 ====================

bool FanqieDownloader::selfUpdate(bool autoConfirm, std::string& output) {
    output.clear();

    std::string exe = locateExe();
    if (exe.empty()) {
        output = "未找到下载器";
        return false;
    }

#ifdef _WIN32
    std::string args = autoConfirm ? "--self-update --self-update-yes" : "--self-update";
    std::string cmdLine = "\"" + exe + "\" " + args;
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE rdPipe = NULL, wrPipe = NULL;
    if (!CreatePipe(&rdPipe, &wrPipe, &sa, 0)) {
        output = "创建管道失败";
        return false;
    }
    SetHandleInformation(rdPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wrPipe;
    si.hStdError = wrPipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string workDir = fsutil::parent(exe);

    BOOL ok = CreateProcessA(
        exe.c_str(), cmdBuf.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, workDir.c_str(), &si, &pi);

    CloseHandle(wrPipe);

    if (!ok) {
        CloseHandle(rdPipe);
        output = "启动更新进程失败";
        return false;
    }

    // 读取输出
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rdPipe, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        buf[got] = '\0';
        output += buf;
    }

    WaitForSingleObject(pi.hProcess, 120000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rdPipe);
    return true;
#else
    (void)autoConfirm;
    output = "自更新仅支持 Windows";
    return false;
#endif
}

std::string FanqieDownloader::downloaderVersion() {
    std::string exe = locateExe();
    if (exe.empty()) return "";

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE rdPipe = NULL, wrPipe = NULL;
    if (!CreatePipe(&rdPipe, &wrPipe, &sa, 0)) return "";
    SetHandleInformation(rdPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wrPipe;
    si.hStdError = wrPipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdLine = "\"" + exe + "\" --version";
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');
    std::string workDir = fsutil::parent(exe);

    BOOL ok = CreateProcessA(
        exe.c_str(), cmdBuf.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, workDir.c_str(), &si, &pi);

    CloseHandle(wrPipe);
    if (!ok) {
        CloseHandle(rdPipe);
        return "";
    }

    std::string out;
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(rdPipe, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
        buf[got] = '\0';
        out += buf;
    }

    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rdPipe);

    // 从 "Tomato Novel Downloader v2.4.16" 中提取版本号
    size_t vpos = out.find(" v");
    if (vpos != std::string::npos) {
        size_t start = vpos + 2;
        size_t end = out.find_first_of(" \r\n", start);
        if (end == std::string::npos) end = out.size();
        return out.substr(start, end - start);
    }
    return "";
#else
    return "";
#endif
}
