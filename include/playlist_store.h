#ifndef PLAYLIST_STORE_H
#define PLAYLIST_STORE_H

#include "music.h"
#include <string>
#include <vector>

// 歌单（用户自建）
struct Playlist {
    std::string name;
    std::vector<std::string> songPaths;   // 按加入顺序
};

// 播放历史条目
struct HistoryEntry {
    std::string path;
    std::string title;
    std::string artist;
    long long playedAtMs = 0;
};

// 音乐相关持久化：歌单、收藏、历史、扫描目录
class MusicStore {
public:
    bool load(const std::string& dir);   // dir 为空则用当前目录
    bool save();

    // ---- 扫描目录 ----
    const std::vector<std::string>& scanDirs() const { return dirs; }
    void addScanDir(const std::string& d);
    bool removeScanDir(const std::string& d);

    // ---- 歌单 ----
    std::vector<Playlist>& playlists() { return pls; }
    const std::vector<Playlist>& playlists() const { return pls; }
    int createPlaylist(const std::string& name);          // 返回索引，失败 -1
    bool renamePlaylist(int idx, const std::string& name);
    bool deletePlaylist(int idx);
    bool addToPlaylist(int idx, const std::vector<std::string>& paths);
    bool removeFromPlaylist(int idx, const std::string& path);
    bool moveInPlaylist(int idx, int from, int to);

    // ---- 收藏 ----
    const std::vector<std::string>& favorites() const { return favs; }
    bool isFavorite(const std::string& path) const;
    bool toggleFavorite(const std::string& path);

    // ---- 播放历史 ----
    const std::vector<HistoryEntry>& history() const { return hist; }
    void recordPlay(const Song& s);
    void clearHistory();

    const std::string& path() const { return storePath; }

    // 从历史/歌单里移除某首歌（文件被删时调用）
    void forgetSong(const std::string& path);

private:
    std::string storePath;
    std::vector<std::string> dirs;
    std::vector<Playlist> pls;
    std::vector<std::string> favs;
    std::vector<HistoryEntry> hist;

    void ensureLoaded();
};

#endif
