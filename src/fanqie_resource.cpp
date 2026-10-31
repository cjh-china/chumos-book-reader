#include "fanqie_resource.h"
#include "fs_util.h"

#include <cstdlib>
#include <vector>

#ifdef HAS_EMBEDDED_FANQIE
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#endif

namespace fanqie_res {

namespace {

// 当前平台的下载器文件名
const char* exeName() {
#ifdef _WIN32
    return "TomatoNovelDownloader.exe";
#else
    return "TomatoNovelDownloader";
#endif
}

// 当前平台的子目录名
const char* platformDir() {
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

} // namespace

#ifdef HAS_EMBEDDED_FANQIE

// ---- 内嵌模式：资源读取 ----

#ifdef _WIN32

namespace {
const int RES_EXE = 1;
const int RES_CFG = 2;

bool loadResource(int id, std::vector<char>& out) {
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(id), RT_RCDATA);
    if (!hRes) return false;
    DWORD size = SizeofResource(NULL, hRes);
    if (size == 0) return false;
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return false;
    void* pData = LockResource(hData);
    if (!pData) return false;
    out.assign((char*)pData, (char*)pData + size);
    return true;
}
} // namespace

bool hasEmbedded() {
    return FindResourceA(NULL, MAKEINTRESOURCEA(RES_EXE), RT_RCDATA) != NULL;
}

size_t embeddedExeSize() {
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(RES_EXE), RT_RCDATA);
    if (!hRes) return 0;
    return (size_t)SizeofResource(NULL, hRes);
}

bool extractTo(const std::string& targetDir) {
    std::vector<char> exeData, cfgData;
    if (!loadResource(RES_EXE, exeData) || exeData.empty()) return false;

    fsutil::createDirs(targetDir);

    std::string exePath = fsutil::join(targetDir, exeName());
    bool needWrite = true;
    if (fsutil::isFile(exePath)) {
        long long sz = fsutil::fileSize(exePath);
        if (sz >= 0 && (size_t)sz == exeData.size()) needWrite = false;
    }
    if (needWrite) {
        if (!fsutil::writeAll(exePath, std::string(exeData.begin(), exeData.end()))) return false;
    }

    if (loadResource(RES_CFG, cfgData) && !cfgData.empty()) {
        std::string cfgPath = fsutil::join(targetDir, "config.yml");
        if (!fsutil::exists(cfgPath)) {
            fsutil::writeAll(cfgPath, std::string(cfgData.begin(), cfgData.end()));
        }
    }
    return true;
}

#else  // POSIX

extern "C" {
    extern const unsigned char _binary_TomatoNovelDownloader_start[];
    extern const unsigned char _binary_TomatoNovelDownloader_end[];
}

bool hasEmbedded() {
    return (_binary_TomatoNovelDownloader_end - _binary_TomatoNovelDownloader_start) > 0;
}

size_t embeddedExeSize() {
    return (size_t)(_binary_TomatoNovelDownloader_end - _binary_TomatoNovelDownloader_start);
}

bool extractTo(const std::string& targetDir) {
    size_t size = (size_t)(_binary_TomatoNovelDownloader_end - _binary_TomatoNovelDownloader_start);
    if (size == 0) return false;

    fsutil::createDirs(targetDir);
    std::string exePath = fsutil::join(targetDir, exeName());

    bool needWrite = true;
    if (fsutil::isFile(exePath)) {
        long long sz = fsutil::fileSize(exePath);
        if (sz >= 0 && (size_t)sz == size) needWrite = false;
    }
    if (needWrite) {
        std::string data((const char*)_binary_TomatoNovelDownloader_start, size);
        if (!fsutil::writeAll(exePath, data)) return false;
    }
    ::chmod(exePath.c_str(), 0755);
    return true;
}

#endif  // _WIN32

// 内嵌模式的释放目录（用户可写）
std::string runtimeDir() {
    const char* ov = std::getenv("FANQIE_RUNTIME_DIR");
    if (ov && *ov) {
        fsutil::createDirs(ov);
        return ov;
    }
#ifdef _WIN32
    const char* local = std::getenv("LOCALAPPDATA");
    if (local && *local) {
        std::string p = fsutil::join(fsutil::join(local, "ebook_reader"), "tomato");
        fsutil::createDirs(p);
        return p;
    }
    std::string tmp = fsutil::join(fsutil::normalize("C:\\Windows\\Temp"), "ebook_reader_tomato");
    fsutil::createDirs(tmp);
    return tmp;
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::string p = fsutil::join(fsutil::join(fsutil::join(home, ".local"), "share"), "ebook_reader");
        p = fsutil::join(p, "tomato");
        fsutil::createDirs(p);
        return p;
    }
    std::string tmp = fsutil::join("/tmp", "ebook_reader_tomato");
    fsutil::createDirs(tmp);
    return tmp;
#endif
}

#endif  // HAS_EMBEDDED_FANQIE

namespace {

// 按优先级列出所有候选目录（外置查找）
std::vector<std::string> candidateDirs() {
    std::vector<std::string> dirs;

    // 1. 环境变量覆盖
    const char* env = std::getenv("FANQIE_TOOL_DIR");
    if (env && *env) dirs.push_back(env);

    // 2. exe 同目录（主要方案）
    std::string base = fsutil::exeDir();
    if (!base.empty()) {
        dirs.push_back(base);
        dirs.push_back(fsutil::join(base, "tomato"));
        dirs.push_back(fsutil::join(base, "tools"));
        dirs.push_back(fsutil::join(fsutil::join(base, "tools"), "tomato"));
    }

    // 3. 从 exe 目录向上找（开发时 exe 在 build_xxx/ 里）
    std::string cur = base;
    for (int i = 0; i < 4 && !cur.empty(); i++) {
        std::string up = fsutil::parent(cur);
        if (up == cur || up.empty()) break;
        cur = up;
        dirs.push_back(fsutil::join(fsutil::join(cur, "tools"), "tomato"));
    }

    // 4. 当前工作目录
    dirs.push_back(".");
    dirs.push_back("tools/tomato");

    return dirs;
}

// 在给定目录下查找下载器，返回完整路径或空
std::string findIn(const std::string& d) {
    if (d.empty() || !fsutil::isDirectory(d)) return "";

    std::string sub = fsutil::join(d, platformDir());
    std::string p = fsutil::join(sub, exeName());
    if (fsutil::isFile(p)) return p;

    p = fsutil::join(d, exeName());
    if (fsutil::isFile(p)) return p;

    return "";
}

} // namespace

// ==================== 对外接口 ====================

std::string locateDownloader() {
    // 1. 优先外置：用户可能放了更新版本的下载器
    for (const auto& d : candidateDirs()) {
        std::string p = findIn(d);
        if (!p.empty()) return p;
    }

#ifdef HAS_EMBEDDED_FANQIE
    // 2. 外置找不到时，释放内嵌资源
    std::string dir = runtimeDir();
    if (!dir.empty() && extractTo(dir)) {
        std::string p = fsutil::join(dir, exeName());
        if (fsutil::isFile(p)) return p;
    }
#endif

    return "";
}

std::string downloaderDir() {
    std::string exe = locateDownloader();
    if (exe.empty()) return "";
    return fsutil::parent(exe);
}

std::string locateConfig() {
    std::string dir = downloaderDir();
    if (dir.empty()) return "";

    // 优先同目录
    std::string p = fsutil::join(dir, "config.yml");
    if (fsutil::isFile(p)) return p;

    // 其次上一级（外置布局时 config.yml 常在工具根目录）
    std::string up = fsutil::parent(dir);
    if (!up.empty() && up != dir) {
        p = fsutil::join(up, "config.yml");
        if (fsutil::isFile(p)) return p;
    }
    return "";
}

bool isAvailable() {
    return !locateDownloader().empty();
}

std::string missingHint() {
#ifdef HAS_EMBEDDED_FANQIE
    return std::string("内嵌下载器释放失败，且未在程序目录找到 ") + exeName() + "。\n"
           "可设置环境变量 FANQIE_RUNTIME_DIR 指定释放目录。";
#else
    return std::string("未找到番茄下载器（") + exeName() + "）。\n"
           "请将 " + exeName() + " 放在程序同目录，或 tools/tomato/ 下。\n"
           "下载地址: https://github.com/zhongbai2333/Tomato-Novel-Downloader/releases";
#endif
}

} // namespace fanqie_res
