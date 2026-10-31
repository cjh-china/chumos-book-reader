#include "config.h"
#include "mini_json.h"
#include "fs_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

const char* CONFIG_NAME = "ebook_reader.config.json";
const size_t MAX_RECENT = 30;

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

bool Config::isBookFile(const std::string& path) {
    std::string ext = fsutil::extension(path);
    return ext == "txt" || ext == "epub" || ext == "docx" || ext == "pdf";
}

std::vector<std::string> Config::scanDir(const std::string& dir, int maxDepth) {
    return fsutil::listBooksRecursive(dir, maxDepth);
}

bool Config::load(const std::string& dir) {
    std::string base = dir;
    if (base.empty()) {
#ifdef _WIN32
        wchar_t wbuf[4096];
        DWORD n = GetCurrentDirectoryW(4096, wbuf);
        if (n > 0 && n < 4096) {
            int need = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, nullptr, 0, nullptr, nullptr);
            if (need > 0) {
                std::string s((size_t)need, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, &s[0], need, nullptr, nullptr);
                base = s;
            }
        }
        if (base.empty()) base = ".";
#else
        base = ".";
#endif
    }
    fsutil::createDirs(base);
    configFilePath = fsutil::join(base, CONFIG_NAME);

    recentFiles.clear();
    libraryDirs.clear();

    std::string content;
    if (!fsutil::readAll(configFilePath, content)) {
        return save();
    }

    minijson::Value root = minijson::parse(content);

    const minijson::Value& rec = root["recent"];
    for (size_t i = 0; i < rec.size(); i++) {
        RecentFile rf;
        rf.path = rec[i]["path"].toStringValue();
        rf.parent = rec[i]["parent"].toStringValue();
        rf.openedAt = (long long)rec[i]["opened_at"].asNumber();
        if (!rf.path.empty()) recentFiles.push_back(rf);
    }

    const minijson::Value& libs = root["library_dirs"];
    for (size_t i = 0; i < libs.size(); i++) {
        std::string d = libs[i].toStringValue();
        if (!d.empty()) libraryDirs.push_back(d);
    }

    lastCursorPath = root["last_cursor"].toStringValue();
    lastCursorOffset_ = (int)root["last_cursor_offset"].asNumber();
    sortMode_ = (int)root["sort_mode"].asNumber();

    // 自动更新开关（缺省视为开启）
    const minijson::Value& au = root["auto_update"];
    if (au.type == minijson::Value::Object) {
        autoUpdateBooks_ = au["books"].type == minijson::Value::Null
                           ? true : au["books"].asBool();
        autoUpdateDownloader_ = au["downloader"].type == minijson::Value::Null
                                ? true : au["downloader"].asBool();
    }

    // so-novel 配置
    soNovelHome_ = root["sonovel_home"].toStringValue();
    const minijson::Value& asn = root["auto_start_sonovel"];
    if (asn.type != minijson::Value::Null) {
        autoStartSoNovel_ = asn.asBool();
    }

    // 书籍来源映射
    bookSources.clear();
    const minijson::Value& src = root["book_sources"];
    if (src.type == minijson::Value::Object) {
        for (const auto& kv : src.obj) {
            std::string id = kv.second.toStringValue();
            if (!kv.first.empty() && !id.empty()) {
                bookSources.emplace_back(kv.first, id);
            }
        }
    }

    return true;
}

bool Config::save() {
    if (configFilePath.empty()) return false;

    std::string os;
    os += "{\n";
    os += "  \"version\": 1,\n";

    os += "  \"recent\": [\n";
    for (size_t i = 0; i < recentFiles.size(); i++) {
        const auto& rf = recentFiles[i];
        os += "    {\"path\": \"" + jsonEscape(rf.path) + "\", ";
        os += "\"parent\": \"" + jsonEscape(rf.parent) + "\", ";
        os += "\"opened_at\": " + std::to_string(rf.openedAt) + "}";
        if (i + 1 < recentFiles.size()) os += ",";
        os += "\n";
    }
    os += "  ],\n";

    os += "  \"library_dirs\": [\n";
    for (size_t i = 0; i < libraryDirs.size(); i++) {
        os += "    \"" + jsonEscape(libraryDirs[i]) + "\"";
        if (i + 1 < libraryDirs.size()) os += ",";
        os += "\n";
    }
    os += "  ],\n";

    os += "  \"last_cursor\": \"" + jsonEscape(lastCursorPath) + "\",\n";
    os += "  \"last_cursor_offset\": " + std::to_string(lastCursorOffset_) + ",\n";
    os += "  \"sort_mode\": " + std::to_string(sortMode_) + ",\n";

    os += "  \"auto_update\": {\n";
    os += "    \"books\": " + std::string(autoUpdateBooks_ ? "true" : "false") + ",\n";
    os += "    \"downloader\": " + std::string(autoUpdateDownloader_ ? "true" : "false") + "\n";
    os += "  },\n";

    os += "  \"sonovel_home\": \"" + jsonEscape(soNovelHome_) + "\",\n";
    os += "  \"auto_start_sonovel\": " + std::string(autoStartSoNovel_ ? "true" : "false") + ",\n";

    os += "  \"book_sources\": {\n";
    for (size_t i = 0; i < bookSources.size(); i++) {
        os += "    \"" + jsonEscape(bookSources[i].first) + "\": \"" +
              jsonEscape(bookSources[i].second) + "\"";
        if (i + 1 < bookSources.size()) os += ",";
        os += "\n";
    }
    os += "  }\n";
    os += "}\n";

    return fsutil::writeAllAtomic(configFilePath, os);
}

void Config::recordFile(const std::string& filePath) {
    if (filePath.empty()) return;

    std::string full = fsutil::normalize(filePath);

    recentFiles.erase(
        std::remove_if(recentFiles.begin(), recentFiles.end(),
                       [&](const RecentFile& r) { return r.path == full; }),
        recentFiles.end());

    RecentFile rf;
    rf.path = full;
    rf.parent = fsutil::parent(full);
    rf.openedAt = nowMs();
    recentFiles.insert(recentFiles.begin(), rf);

    if (recentFiles.size() > MAX_RECENT) recentFiles.resize(MAX_RECENT);

    save();
}

void Config::addLibraryDir(const std::string& dir) {
    if (dir.empty()) return;
    std::string full = fsutil::normalize(dir);

    for (const auto& d : libraryDirs) {
        if (d == full) return;
    }
    libraryDirs.push_back(full);
    save();
}

bool Config::removeLibraryDir(const std::string& dir) {
    auto it = std::find(libraryDirs.begin(), libraryDirs.end(), dir);
    if (it == libraryDirs.end()) return false;
    libraryDirs.erase(it);
    save();
    return true;
}

bool Config::hasLibraryDir(const std::string& dir) const {
    std::string full = fsutil::normalize(dir);
    return std::find(libraryDirs.begin(), libraryDirs.end(), full) != libraryDirs.end();
}

void Config::setBookSource(const std::string& filePath, const std::string& bookId) {
    if (filePath.empty() || bookId.empty()) return;

    std::string full = fsutil::normalize(filePath);

    for (auto& kv : bookSources) {
        if (kv.first == full) {
            if (kv.second == bookId) return;
            kv.second = bookId;
            save();
            return;
        }
    }

    bookSources.emplace_back(full, bookId);
    save();
}

std::string Config::bookSource(const std::string& filePath) const {
    if (filePath.empty()) return "";
    std::string full = fsutil::normalize(filePath);
    for (const auto& kv : bookSources) {
        if (kv.first == full) return kv.second;
    }
    return "";
}
