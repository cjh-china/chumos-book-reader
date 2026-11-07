#include "gui.h"

#include "http_server.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace gui {
namespace {

const char* kFavicon =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
    "<circle cx=\"32\" cy=\"34\" r=\"26\" fill=\"#ff5b3a\"/>"
    "<path d=\"M22 12c4-6 16-6 20 0-6-1-14-1-20 0z\" fill=\"#3ba55d\"/>"
    "<rect x=\"29\" y=\"7\" width=\"6\" height=\"8\" rx=\"3\" fill=\"#2f8a4c\"/>"
    "<path d=\"M20 34h24M22 42h20\" stroke=\"rgba(255,255,255,.65)\" stroke-width=\"3\" "
    "stroke-linecap=\"round\"/>"
    "</svg>";

#ifdef _WIN32

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

std::wstring envWide(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    if (n == 0 || n >= 1024) return std::wstring();
    return std::wstring(buf, n);
}

// 优先 Edge（Windows 自带），其次 Chrome；找不到返回空串。
std::wstring findChromium() {
    const std::wstring edgeRel = L"\\Microsoft\\Edge\\Application\\msedge.exe";
    const std::wstring chromeRel = L"\\Google\\Chrome\\Application\\chrome.exe";

    std::vector<std::wstring> roots;
    std::wstring p;
    p = envWide(L"ProgramFiles(x86)"); if (!p.empty()) roots.push_back(p);
    p = envWide(L"ProgramFiles");      if (!p.empty()) roots.push_back(p);
    p = envWide(L"LOCALAPPDATA");      if (!p.empty()) roots.push_back(p);

    std::vector<std::wstring> candidates;
    for (const auto& r : roots) candidates.push_back(r + edgeRel);
    for (const auto& r : roots) candidates.push_back(r + chromeRel);

    for (const auto& c : candidates) {
        DWORD a = GetFileAttributesW(c.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) return c;
    }
    return std::wstring();
}

// 用独立 profile，避免把 --app 请求转发给已运行的浏览器实例而丢失窗口参数。
std::wstring profileDir() {
    std::wstring base = envWide(L"APPDATA");
    if (base.empty()) return std::wstring();
    std::wstring parent = base + L"\\ebook_reader";
    CreateDirectoryW(parent.c_str(), nullptr);   // 已存在则忽略
    std::wstring dir = parent + L"\\gui_profile";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool launchChromium(const std::wstring& browser, const std::wstring& url) {
    std::wstring profile = profileDir();
    std::wstring cmd = L"\"" + browser + L"\"";
    cmd += L" --app=" + url;
    // 桌面版式（Koodo 风格：侧边导航 + 顶部工具栏 + 中央阅读区）；
    // 窗口宽度小于 900px 时页面会自动回退到手机版布局。
    cmd += L" --window-size=1280,860";
    cmd += L" --no-first-run --no-default-browser-check --disable-extensions";
    if (!profile.empty()) cmd += L" --user-data-dir=\"" + profile + L"\"";

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessW(browser.c_str(), buf.data(), nullptr, nullptr,
                             FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) return false;

    // 正常情况下浏览器主进程会一直存在到窗口关闭。
    // 若 2 秒内就退出，多半是参数被转交给了已有实例：保持服务不退出，改等控制台。
    DWORD first = WaitForSingleObject(pi.hProcess, 2000);
    if (first == WAIT_TIMEOUT) {
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else {
        std::printf("[gui] 浏览器进程已退出；若页面仍在浏览器中，按回车结束服务。\n");
        std::fflush(stdout);
        std::getchar();
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else  // POSIX

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

#endif

} // namespace

void registerRoutes(api::Server& server) {
    server.route("GET", "/", [](const api::Request&) {
        api::Response res;
        res.status = 200;
        res.contentType = "text/html; charset=utf-8";
        res.body = readerPage();
        return res;
    });

    server.route("GET", "/favicon.ico", [](const api::Request&) {
        api::Response res;
        res.status = 200;
        res.contentType = "image/svg+xml";
        res.body = kFavicon;
        return res;
    });
}

bool openWindow(const std::string& host, int port) {
    std::string hostForUrl = host.empty() ? "127.0.0.1" : host;
    if (hostForUrl == "0.0.0.0") hostForUrl = "127.0.0.1";

    std::string url = "http://" + hostForUrl + ":" + std::to_string(port) + "/";
    std::printf("[gui] 阅读器地址: %s\n", url.c_str());
    std::fflush(stdout);

#ifdef _WIN32
    std::wstring wideUrl = utf8ToWide(url);
    std::wstring browser = findChromium();
    if (!browser.empty() && launchChromium(browser, wideUrl)) return true;

    // 降级：交给系统默认浏览器（普通标签页），并保持服务直到用户按回车。
    std::printf("[gui] 未找到 Edge/Chrome，改用系统默认浏览器。\n");
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wideUrl.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        std::fprintf(stderr, "[gui] 无法打开浏览器，请手动访问 %s\n", url.c_str());
    }
    std::printf("[gui] 阅读完毕后按回车结束服务。\n");
    std::fflush(stdout);
    std::getchar();
    return true;
#else
    std::string cmd;
#ifdef __APPLE__
    cmd = "open " + shellQuote(url) + " >/dev/null 2>&1 &";
#else
    cmd = "xdg-open " + shellQuote(url) + " >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "[gui] 无法打开浏览器，请手动访问 %s\n", url.c_str());
    }
    std::printf("[gui] 阅读完毕后按回车结束服务。\n");
    std::fflush(stdout);
    std::getchar();
    return true;
#endif
}

} // namespace gui
