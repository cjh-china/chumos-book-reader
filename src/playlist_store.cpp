#include "playlist_store.h"
#include "fs_util.h"
#include "mini_json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace {

const char* STORE_NAME = "music_store.json";
const size_t MAX_HISTORY = 500;

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

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

std::string normPath(const std::string& p) {
    return fsutil::normalize(p);
}

} // namespace

bool MusicStore::load(const std::string& dir) {
    std::string base = dir;
    if (base.empty()) base = ".";
    fsutil::createDirs(base);
    storePath = fsutil::join(base, STORE_NAME);

    dirs.clear();
    pls.clear();
    favs.clear();
    hist.clear();

    std::string content;
    if (!fsutil::readAll(storePath, content)) {
        return save();   // 首次运行创建空文件
    }

    minijson::Value root = minijson::parse(content);
    if (root.type != minijson::Value::Object) return true;   // 损坏则当作空

    const minijson::Value& d = root["scan_dirs"];
    for (size_t i = 0; i < d.size(); i++) {
        std::string v = d[i].toStringValue();
        if (!v.empty()) dirs.push_back(v);
    }

    const minijson::Value& ps = root["playlists"];
    for (size_t i = 0; i < ps.size(); i++) {
        Playlist p;
        p.name = ps[i]["name"].toStringValue();
        if (p.name.empty()) continue;
        const minijson::Value& songs = ps[i]["songs"];
        for (size_t k = 0; k < songs.size(); k++) {
            std::string s = songs[k].toStringValue();
            if (!s.empty()) p.songPaths.push_back(s);
        }
        pls.push_back(std::move(p));
    }

    const minijson::Value& fv = root["favorites"];
    for (size_t i = 0; i < fv.size(); i++) {
        std::string v = fv[i].toStringValue();
        if (!v.empty()) favs.push_back(v);
    }

    const minijson::Value& hs = root["history"];
    for (size_t i = 0; i < hs.size(); i++) {
        HistoryEntry h;
        h.path = hs[i]["path"].toStringValue();
        h.title = hs[i]["title"].toStringValue();
        h.artist = hs[i]["artist"].toStringValue();
        h.playedAtMs = (long long)hs[i]["played_at"].asNumber();
        if (!h.path.empty()) hist.push_back(std::move(h));
    }

    return true;
}

bool MusicStore::save() {
    if (storePath.empty()) return false;

    std::ostringstream os;
    os << "{\n  \"version\": 1,\n";

    os << "  \"scan_dirs\": [\n";
    for (size_t i = 0; i < dirs.size(); i++) {
        os << "    \"" << jsonEscape(dirs[i]) << "\"";
        if (i + 1 < dirs.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"playlists\": [\n";
    for (size_t i = 0; i < pls.size(); i++) {
        os << "    {\"name\": \"" << jsonEscape(pls[i].name) << "\", \"songs\": [";
        for (size_t k = 0; k < pls[i].songPaths.size(); k++) {
            os << "\"" << jsonEscape(pls[i].songPaths[k]) << "\"";
            if (k + 1 < pls[i].songPaths.size()) os << ", ";
        }
        os << "]}";
        if (i + 1 < pls.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"favorites\": [\n";
    for (size_t i = 0; i < favs.size(); i++) {
        os << "    \"" << jsonEscape(favs[i]) << "\"";
        if (i + 1 < favs.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"history\": [\n";
    for (size_t i = 0; i < hist.size(); i++) {
        os << "    {\"path\": \"" << jsonEscape(hist[i].path)
           << "\", \"title\": \"" << jsonEscape(hist[i].title)
           << "\", \"artist\": \"" << jsonEscape(hist[i].artist)
           << "\", \"played_at\": " << hist[i].playedAtMs << "}";
        if (i + 1 < hist.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n";

    os << "}\n";
    return fsutil::writeAllAtomic(storePath, os.str());
}

void MusicStore::ensureLoaded() {
    if (storePath.empty()) load("");
}

// ==================== 扫描目录 ====================

void MusicStore::addScanDir(const std::string& d) {
    if (d.empty()) return;
    std::string n = normPath(d);
    for (const auto& x : dirs) {
        if (normPath(x) == n) return;
    }
    dirs.push_back(d);
    save();
}

bool MusicStore::removeScanDir(const std::string& d) {
    std::string n = normPath(d);
    auto it = std::find_if(dirs.begin(), dirs.end(),
                           [&](const std::string& x) { return normPath(x) == n; });
    if (it == dirs.end()) return false;
    dirs.erase(it);
    save();
    return true;
}

// ==================== 歌单 ====================

int MusicStore::createPlaylist(const std::string& name) {
    if (name.empty()) return -1;
    for (const auto& p : pls) {
        if (p.name == name) return -1;   // 重名
    }
    Playlist p;
    p.name = name;
    pls.push_back(std::move(p));
    save();
    return (int)pls.size() - 1;
}

bool MusicStore::renamePlaylist(int idx, const std::string& name) {
    if (idx < 0 || idx >= (int)pls.size() || name.empty()) return false;
    for (size_t i = 0; i < pls.size(); i++) {
        if ((int)i != idx && pls[i].name == name) return false;
    }
    pls[idx].name = name;
    save();
    return true;
}

bool MusicStore::deletePlaylist(int idx) {
    if (idx < 0 || idx >= (int)pls.size()) return false;
    pls.erase(pls.begin() + idx);
    save();
    return true;
}

bool MusicStore::addToPlaylist(int idx, const std::vector<std::string>& paths) {
    if (idx < 0 || idx >= (int)pls.size()) return false;
    auto& target = pls[idx].songPaths;
    bool changed = false;
    for (const auto& raw : paths) {
        std::string p = normPath(raw);
        if (p.empty()) continue;
        bool exists = false;
        for (const auto& x : target) {
            if (normPath(x) == p) { exists = true; break; }
        }
        if (!exists) {
            target.push_back(raw);
            changed = true;
        }
    }
    if (changed) save();
    return changed;
}

bool MusicStore::removeFromPlaylist(int idx, const std::string& path) {
    if (idx < 0 || idx >= (int)pls.size()) return false;
    std::string n = normPath(path);
    auto& v = pls[idx].songPaths;
    auto it = std::find_if(v.begin(), v.end(),
                           [&](const std::string& x) { return normPath(x) == n; });
    if (it == v.end()) return false;
    v.erase(it);
    save();
    return true;
}

bool MusicStore::moveInPlaylist(int idx, int from, int to) {
    if (idx < 0 || idx >= (int)pls.size()) return false;
    auto& v = pls[idx].songPaths;
    if (from < 0 || from >= (int)v.size()) return false;
    if (to < 0 || to >= (int)v.size()) return false;
    if (from == to) return true;
    std::string tmp = v[from];
    v.erase(v.begin() + from);
    v.insert(v.begin() + to, tmp);
    save();
    return true;
}

// ==================== 收藏 ====================

bool MusicStore::isFavorite(const std::string& path) const {
    std::string n = normPath(path);
    for (const auto& x : favs) {
        if (normPath(x) == n) return true;
    }
    return false;
}

bool MusicStore::toggleFavorite(const std::string& path) {
    std::string n = normPath(path);
    auto it = std::find_if(favs.begin(), favs.end(),
                           [&](const std::string& x) { return normPath(x) == n; });
    if (it != favs.end()) {
        favs.erase(it);
        save();
        return false;
    }
    favs.push_back(path);
    save();
    return true;
}

// ==================== 历史 ====================

void MusicStore::recordPlay(const Song& s) {
    std::string n = normPath(s.path);

    hist.erase(std::remove_if(hist.begin(), hist.end(),
                              [&](const HistoryEntry& h) { return normPath(h.path) == n; }),
               hist.end());

    HistoryEntry h;
    h.path = s.path;
    h.title = s.title;
    h.artist = s.artist;
    h.playedAtMs = nowMs();
    hist.insert(hist.begin(), std::move(h));

    if (hist.size() > MAX_HISTORY) hist.resize(MAX_HISTORY);
    save();
}

void MusicStore::clearHistory() {
    hist.clear();
    save();
}

void MusicStore::forgetSong(const std::string& path) {
    std::string n = normPath(path);
    bool changed = false;

    for (auto& p : pls) {
        size_t before = p.songPaths.size();
        p.songPaths.erase(
            std::remove_if(p.songPaths.begin(), p.songPaths.end(),
                           [&](const std::string& x) { return normPath(x) == n; }),
            p.songPaths.end());
        if (p.songPaths.size() != before) changed = true;
    }

    size_t fb = favs.size();
    favs.erase(std::remove_if(favs.begin(), favs.end(),
                              [&](const std::string& x) { return normPath(x) == n; }),
               favs.end());
    if (favs.size() != fb) changed = true;

    size_t hb = hist.size();
    hist.erase(std::remove_if(hist.begin(), hist.end(),
                              [&](const HistoryEntry& h) { return normPath(h.path) == n; }),
               hist.end());
    if (hist.size() != hb) changed = true;

    if (changed) save();
}
