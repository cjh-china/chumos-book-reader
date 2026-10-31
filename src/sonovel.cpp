#include "sonovel.h"
#include "http_client.h"
#include "mini_json.h"
#include "fs_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string urlEncode(const std::string& s) {
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

} // namespace

SoNovel::SoNovel() {}

std::string SoNovel::baseUrl() {
    const char* env = std::getenv("SONOVEL_API_BASE");
    if (env && *env) {
        std::string b = env;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }
    return "http://127.0.0.1:7765";
}

std::string SoNovel::serverDownloadDir() {
    // so-novel 默认把书存在其工作目录的 downloads/ 下。
    // 这里只在用户把 sonovel 放在 exe 同目录时有效，仅用于快速定位；
    // 实际取文件走 HTTP /book-download，不依赖此路径。
    std::string base = fsutil::exeDir();
    if (base.empty()) base = ".";
    return fsutil::join(base, "downloads");
}

std::string SoNovel::httpGet(const std::string& path, int timeoutSec) {
    HttpClient client;
    client.setTimeout(timeoutSec);
    std::map<std::string, std::string> headers;
    headers["User-Agent"] = "Mozilla/5.0 (ebook_reader)";
    return client.get(baseUrl() + path, headers);
}

bool SoNovel::isAvailable() {
    HttpClient client;
    client.setTimeout(3);
    std::string r = client.get(baseUrl() + "/config");
    return !r.empty() && r.find("\"code\"") != std::string::npos;
}

std::vector<SoNovel::Book> SoNovel::search(const std::string& keyword) {
    std::vector<Book> out;
    errMsg.clear();
    if (keyword.empty()) return out;

    // 聚合搜索可能很慢，给足超时
    std::string body = httpGet("/search/aggregated?kw=" + urlEncode(keyword), 90);
    if (body.empty()) {
        errMsg = "搜索失败（服务未响应）";
        return out;
    }

    minijson::Value root = minijson::parse(body);
    if ((int)root["code"].asNumber() != 200) {
        errMsg = "搜索失败（服务返回错误）";
        return out;
    }

    const minijson::Value& data = root["data"];
    for (size_t i = 0; i < data.size(); i++) {
        const minijson::Value& it = data[i];

        Book b;
        b.sourceId = (int)it["sourceId"].asNumber();
        b.sourceName = it["sourceName"].toStringValue();
        b.url = it["url"].toStringValue();
        b.bookName = it["bookName"].toStringValue();
        b.author = it["author"].toStringValue();
        b.category = it["category"].toStringValue();
        b.latestChapter = it["latestChapter"].toStringValue();
        b.lastUpdateTime = it["lastUpdateTime"].toStringValue();
        b.status = it["status"].toStringValue();

        if (b.bookName.empty() || b.url.empty()) continue;
        out.push_back(std::move(b));
    }

    if (out.empty() && errMsg.empty()) errMsg = "没有搜索结果";
    return out;
}

std::string SoNovel::download(const Book& book, const std::string& format) {
    errMsg.clear();

    // 记录下载前的文件列表，用于识别新文件
    std::vector<std::string> before = listDownloaded();

    std::string q = "sourceId=" + std::to_string(book.sourceId)
                  + "&url=" + urlEncode(book.url)
                  + "&bookName=" + urlEncode(book.bookName)
                  + "&author=" + urlEncode(book.author);
    if (!format.empty()) q += "&format=" + urlEncode(format);

    // 下载可能很久（几百章），超时给到 10 分钟
    std::string resp = httpGet("/book-fetch?" + q, 600);

    // 成功后服务端返回 200 + 空体；失败返回 JSON 错误
    if (!resp.empty() && resp.find("\"code\"") != std::string::npos) {
        minijson::Value j = minijson::parse(resp);
        std::string msg = j["message"].toStringValue();
        errMsg = msg.empty() ? "下载失败" : msg;
        return "";
    }

    // 对比列表找新文件
    std::vector<std::string> after = listDownloaded();
    for (const auto& name : after) {
        if (std::find(before.begin(), before.end(), name) == before.end()) {
            return name;
        }
    }

    errMsg = "下载完成但未能定位生成的文件";
    return "";
}

std::vector<std::string> SoNovel::listDownloaded() {
    std::vector<std::string> out;

    std::string body = httpGet("/local-books", 15);
    if (body.empty()) return out;

    minijson::Value root = minijson::parse(body);
    if ((int)root["code"].asNumber() != 200) return out;

    const minijson::Value& data = root["data"];
    for (size_t i = 0; i < data.size(); i++) {
        std::string name = data[i]["name"].toStringValue();
        if (!name.empty()) out.push_back(name);
    }
    return out;
}

bool SoNovel::fetchToLocal(const std::string& fileName, const std::string& localDir,
                           std::string& outPath) {
    outPath.clear();
    if (fileName.empty()) {
        errMsg = "文件名为空";
        return false;
    }

    std::string body = httpGet("/book-download?filename=" + urlEncode(fileName), 120);
    if (body.empty()) {
        errMsg = "下载文件失败";
        return false;
    }

    fsutil::createDirs(localDir);
    std::string path = fsutil::join(localDir, fileName);
    if (!fsutil::writeAll(path, body)) {
        errMsg = "写入本地文件失败";
        return false;
    }

    outPath = path;
    return true;
}

// ==================== 服务管理 ====================

std::string SoNovel::findHome(const std::string& hint) {
    std::vector<std::string> candidates;

    if (!hint.empty()) candidates.push_back(hint);

    // 环境变量
    const char* env = std::getenv("SONOVEL_HOME");
    if (env && *env) candidates.push_back(env);

    // exe 同目录及其子目录
    std::string base = fsutil::exeDir();
    if (!base.empty()) {
        candidates.push_back(base);
        candidates.push_back(fsutil::join(base, "sonovel"));
        candidates.push_back(fsutil::join(base, "tools"));
        candidates.push_back(fsutil::join(fsutil::join(base, "tools"), "sonovel"));
    }

    // 从 exe 目录向上找
    std::string cur = base;
    for (int i = 0; i < 4 && !cur.empty(); i++) {
        std::string up = fsutil::parent(cur);
        if (up == cur || up.empty()) break;
        cur = up;
        candidates.push_back(fsutil::join(cur, "sonovel-windows"));
        candidates.push_back(fsutil::join(fsutil::join(cur, "tools"), "sonovel"));
    }

    // D 盘常见位置（Windows）
#ifdef _WIN32
    candidates.push_back("D:\\sonovel-windows");
    candidates.push_back("C:\\sonovel-windows");
#endif

    candidates.push_back(".");

#ifdef _WIN32
    const char* exeName = "sonovel.exe";
#else
    const char* exeName = "sonovel";
#endif

    for (const auto& d : candidates) {
        if (d.empty() || !fsutil::isDirectory(d)) continue;
        if (fsutil::isFile(fsutil::join(d, exeName))) return d;
    }
    return "";
}

bool SoNovel::ensureService(const std::string& homeDir, int waitSeconds) {
    if (isAvailable()) return true;

    std::string home = findHome(homeDir);
    if (home.empty()) {
        errMsg = "未找到 sonovel.exe";
        return false;
    }

#ifdef _WIN32
    std::string exe = fsutil::join(home, "sonovel.exe");
    std::string cmd = "\"" + exe + "\"";
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(
        exe.c_str(), cmdBuf.data(), NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, home.c_str(), &si, &pi);

    if (!ok) {
        errMsg = "启动 sonovel.exe 失败";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    std::string exe = fsutil::join(home, "sonovel");
    std::string cmd = "cd \"" + home + "\" && nohup \"" + exe + "\" >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {
        errMsg = "启动 sonovel 失败";
        return false;
    }
#endif

    // 等待服务就绪
    int tries = waitSeconds * 2;
    for (int i = 0; i < tries; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (isAvailable()) return true;
    }

    errMsg = "sonovel 启动超时（可能是首次启动需解压 runtime）";
    return false;
}
