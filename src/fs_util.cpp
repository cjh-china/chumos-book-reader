#include "fs_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>
#endif

namespace fsutil {

namespace {

bool isSep(char c) { return c == '/' || c == '\\'; }

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool isBookExt(const std::string& ext) {
    return ext == "txt" || ext == "epub" || ext == "docx" || ext == "pdf";
}

#ifdef _WIN32

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w((size_t)need, L'\0');
    int got = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], need);
    if (got <= 0) return std::wstring();
    w.resize((size_t)got);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string s((size_t)need, '\0');
    int got = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  &s[0], need, nullptr, nullptr);
    if (got <= 0) return std::string();
    s.resize((size_t)got);
    return s;
}

#endif

} // namespace

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (isSep(a.back())) return a + b;
    char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    return a + sep + b;
}

std::string parent(const std::string& path) {
    if (path.empty()) return path;
    size_t end = path.size();
    while (end > 0 && isSep(path[end - 1])) end--;
    if (end == 0) return path.substr(0, 1);
    size_t pos = end;
    while (pos > 0 && !isSep(path[pos - 1])) pos--;
    if (pos == 0) return "";
    size_t cut = pos;
    while (cut > 1 && isSep(path[cut - 1])) cut--;
    // Windows 盘符根："C:\a.txt" -> "C:\"
#ifdef _WIN32
    if (cut == 2 && path.size() >= 2 && path[1] == ':') {
        return path.substr(0, 3);
    }
#endif
    return path.substr(0, cut);
}

std::string filename(const std::string& path) {
    if (path.empty()) return path;
    size_t end = path.size();
    while (end > 0 && isSep(path[end - 1])) end--;
    if (end == 0) return "";
    size_t pos = end;
    while (pos > 0 && !isSep(path[pos - 1])) pos--;
    return path.substr(pos, end - pos);
}

std::string extension(const std::string& path) {
    std::string name = filename(path);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return "";
    return toLowerAscii(name.substr(dot + 1));
}

std::string stem(const std::string& path) {
    std::string name = filename(path);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return name;
    return name.substr(0, dot);
}

std::string normalize(const std::string& path) {
    if (path.empty()) return path;

#ifdef _WIN32
    const char sep = '\\';
    const char altSep = '/';
#else
    const char sep = '/';
    const char altSep = '\\';
#endif

    // 统一为主分隔符
    std::string s = path;
    std::replace(s.begin(), s.end(), altSep, sep);

    // 折叠连续分隔符（保留开头的 UNC 双分隔符 / 根路径）
    std::string out;
    out.reserve(s.size());
    bool prevSep = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == sep) {
            // 允许开头两个分隔符（UNC 或 //）
            if (prevSep && !(i == 1 && s[0] == sep)) continue;
            prevSep = true;
            out += c;
        } else {
            prevSep = false;
            out += c;
        }
    }

    // 去掉尾部多余分隔符，但保留根
    while (out.size() > 1 && out.back() == sep) out.pop_back();
    return out;
}

std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::wstring w(buf, n);
    size_t pos = w.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return "";
    return wideToUtf8(w.substr(0, pos));
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string s(buf);
    size_t pos = s.find_last_of('/');
    if (pos == std::string::npos) return "";
    return s.substr(0, pos);
#endif
}

#ifdef _WIN32

bool exists(const std::string& path) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return false;
    DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool isDirectory(const std::string& path) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return false;
    DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::string& path) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return false;
    DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

long long fileSize(const std::string& path) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad)) return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
    return ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

long long lastWriteMs(const std::string& path) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER ui;
    ui.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    ui.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    // FILETIME: 100ns 单位，起点 1601-01-01
    const unsigned long long EPOCH_DIFF = 116444736000000000ULL;
    if (ui.QuadPart < EPOCH_DIFF) return 0;
    return (long long)((ui.QuadPart - EPOCH_DIFF) / 10000ULL);
}

bool readAll(const std::string& path, std::string& out) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return false;
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    if (size.QuadPart < 0 || size.QuadPart > (LONGLONG)1 << 31) {
        CloseHandle(h);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD total = 0;
    if (!out.empty()) {
        if (!ReadFile(h, &out[0], (DWORD)out.size(), &total, nullptr)) {
            CloseHandle(h);
            return false;
        }
    }
    CloseHandle(h);
    out.resize(total);
    return true;
}

bool readHead(const std::string& path, size_t maxBytes, std::string& out) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return false;
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }

    size_t toRead = (size_t)size.QuadPart;
    if (toRead > maxBytes) toRead = maxBytes;

    out.resize(toRead);
    DWORD got = 0;
    if (toRead > 0) {
        if (!ReadFile(h, &out[0], (DWORD)toRead, &got, nullptr)) {
            CloseHandle(h);
            return false;
        }
    }
    CloseHandle(h);
    out.resize(got);
    return true;
}

bool writeAll(const std::string& path, const std::string& data) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return false;
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = true;
    if (!data.empty()) {
        ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) &&
             written == (DWORD)data.size();
    }
    CloseHandle(h);
    return ok;
}

bool writeAllAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    if (!writeAll(tmp, data)) {
        removeFile(tmp);
        return false;
    }
    std::wstring wtmp = utf8ToWide(tmp);
    std::wstring wdst = utf8ToWide(path);
    if (wtmp.empty() || wdst.empty()) { removeFile(tmp); return false; }
    if (MoveFileExW(wtmp.c_str(), wdst.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    removeFile(tmp);
    return false;
}

bool removeFile(const std::string& path) {
    std::wstring w = utf8ToWide(path);
    if (w.empty()) return false;
    return DeleteFileW(w.c_str()) != 0;
}

bool createDirs(const std::string& path) {
    std::string p = normalize(path);
    if (p.empty()) return false;
    if (isDirectory(p)) return true;

    // 逐级向上找到已存在的祖先，再逐级创建
    std::vector<std::string> missing;
    std::string cur = p;
    while (!cur.empty() && !isDirectory(cur)) {
        missing.push_back(cur);
        std::string up = parent(cur);
        if (up == cur || up.empty()) break;
        cur = up;
    }
    if (!isDirectory(cur) && !cur.empty()) {
        // 根都不可用
        if (!isDirectory(cur)) {
            std::wstring wcur = utf8ToWide(cur);
            if (!wcur.empty()) CreateDirectoryW(wcur.c_str(), nullptr);
        }
    }

    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        std::wstring w = utf8ToWide(*it);
        if (w.empty()) return false;
        if (!CreateDirectoryW(w.c_str(), nullptr)) {
            if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return isDirectory(p);
}

std::vector<std::string> listBooksRecursive(const std::string& dir, int maxDepth) {
    std::vector<std::string> result;

    struct Node { std::string path; int depth; };
    std::vector<Node> stack;
    stack.push_back({dir, 0});

    while (!stack.empty()) {
        Node cur = stack.back();
        stack.pop_back();

        std::string pattern = join(cur.path, "*");
        std::wstring wpat = utf8ToWide(pattern);
        if (wpat.empty()) continue;

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(wpat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            std::wstring name(fd.cFileName);
            if (name == L"." || name == L"..") continue;

            std::string child = join(cur.path, wideToUtf8(name));
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (isDir) {
                if (cur.depth < maxDepth) stack.push_back({child, cur.depth + 1});
            } else {
                if (isBookExt(extension(child))) result.push_back(child);
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> listFilesRecursive(const std::string& dir, int maxDepth) {
    std::vector<std::string> result;

    struct Node { std::string path; int depth; };
    std::vector<Node> stack;
    stack.push_back({dir, 0});

    while (!stack.empty()) {
        Node cur = stack.back();
        stack.pop_back();

        std::string pattern = join(cur.path, "*");
        std::wstring wpat = utf8ToWide(pattern);
        if (wpat.empty()) continue;

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(wpat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            std::wstring name(fd.cFileName);
            if (name == L"." || name == L"..") continue;

            // 跳过隐藏/系统目录（如 .git、System Volume Information）
            DWORD attr = fd.dwFileAttributes;
            bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir && (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;

            std::string child = join(cur.path, wideToUtf8(name));
            if (isDir) {
                if (cur.depth < maxDepth) stack.push_back({child, cur.depth + 1});
            } else {
                result.push_back(child);
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    }

    std::sort(result.begin(), result.end());
    return result;
}

#else  // POSIX

bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

long long fileSize(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long long)st.st_size;
}

long long lastWriteMs(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return (long long)st.st_mtime * 1000LL;
}

bool readAll(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    size_t got = sz > 0 ? std::fread(&out[0], 1, (size_t)sz, f) : 0;
    std::fclose(f);
    out.resize(got);
    return got == (size_t)sz;
}

bool readHead(const std::string& path, size_t maxBytes, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.resize(maxBytes);
    size_t got = std::fread(&out[0], 1, maxBytes, f);
    std::fclose(f);
    out.resize(got);
    return true;
}

bool writeAll(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t w = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return w == data.size();
}

bool writeAllAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    if (!writeAll(tmp, data)) { removeFile(tmp); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { removeFile(tmp); return false; }
    return true;
}

bool removeFile(const std::string& path) {
    return std::remove(path.c_str()) == 0;
}

bool createDirs(const std::string& path) {
    if (path.empty()) return false;
    if (isDirectory(path)) return true;

    std::string cur;
    size_t i = 0;
    if (path[0] == '/') { cur = "/"; i = 1; }

    while (i <= path.size()) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && !isDirectory(cur)) {
                if (::mkdir(cur.c_str(), 0755) != 0 && !isDirectory(cur)) return false;
            }
            if (i < path.size()) cur += '/';
        } else {
            cur += path[i];
        }
        i++;
    }
    return isDirectory(path);
}

std::vector<std::string> listBooksRecursive(const std::string& dir, int maxDepth) {
    std::vector<std::string> result;

    struct Node { std::string path; int depth; };
    std::vector<Node> stack;
    stack.push_back({dir, 0});

    while (!stack.empty()) {
        Node cur = stack.back();
        stack.pop_back();

        DIR* d = ::opendir(cur.path.c_str());
        if (!d) continue;

        struct dirent* ent;
        while ((ent = ::readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;

            std::string child = join(cur.path, name);
            if (isDirectory(child)) {
                if (cur.depth < maxDepth) stack.push_back({child, cur.depth + 1});
            } else if (isBookExt(extension(child))) {
                result.push_back(child);
            }
        }
        ::closedir(d);
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> listFilesRecursive(const std::string& dir, int maxDepth) {
    std::vector<std::string> result;

    struct Node { std::string path; int depth; };
    std::vector<Node> stack;
    stack.push_back({dir, 0});

    while (!stack.empty()) {
        Node cur = stack.back();
        stack.pop_back();

        DIR* d = ::opendir(cur.path.c_str());
        if (!d) continue;

        struct dirent* ent;
        while ((ent = ::readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            if (!name.empty() && name[0] == '.') continue;   // 跳过隐藏项

            std::string child = join(cur.path, name);
            if (isDirectory(child)) {
                if (cur.depth < maxDepth) stack.push_back({child, cur.depth + 1});
            } else {
                result.push_back(child);
            }
        }
        ::closedir(d);
    }

    std::sort(result.begin(), result.end());
    return result;
}

#endif

} // namespace fsutil
