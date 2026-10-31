#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <utility>
#include <vector>

struct RecentFile {
    std::string path;
    std::string parent;
    long long openedAt = 0;
};

class Config {
public:
    bool load(const std::string& dir = "");
    bool save();

    void recordFile(const std::string& filePath);

    void addLibraryDir(const std::string& dir);
    bool removeLibraryDir(const std::string& dir);
    bool hasLibraryDir(const std::string& dir) const;

    const std::vector<RecentFile>& recent() const { return recentFiles; }
    const std::vector<std::string>& dirs() const { return libraryDirs; }
    const std::string& path() const { return configFilePath; }

    // 上次浏览位置（选中项路径 + 滚动偏移）
    void setLastCursor(const std::string& path, int offset) {
        if (lastCursorPath == path && lastCursorOffset_ == offset) return;
        lastCursorPath = path;
        lastCursorOffset_ = offset;
        save();
    }
    const std::string& lastCursor() const { return lastCursorPath; }
    int lastCursorOffset() const { return lastCursorOffset_; }

    // 书库排序方式：0=名称 1=大小 2=修改时间
    int sortMode() const { return sortMode_; }
    void setSortMode(int m) { sortMode_ = m; save(); }

    // 打开番茄书籍时是否自动检查更新（默认开启）
    bool autoUpdateBooks() const { return autoUpdateBooks_; }
    void setAutoUpdateBooks(bool on) { autoUpdateBooks_ = on; save(); }

    // 启动时是否检查下载器更新（默认开启）
    bool autoUpdateDownloader() const { return autoUpdateDownloader_; }
    void setAutoUpdateDownloader(bool on) { autoUpdateDownloader_ = on; save(); }

    // 记录某本书的番茄 book_id（用于后续更新）
    void setBookSource(const std::string& filePath, const std::string& bookId);
    std::string bookSource(const std::string& filePath) const;

    // so-novel 安装目录（含 sonovel.exe）。为空则自动探测。
    const std::string& soNovelHome() const { return soNovelHome_; }
    void setSoNovelHome(const std::string& dir) { soNovelHome_ = dir; save(); }

    // 是否自动拉起 so-novel 服务（默认开启）
    bool autoStartSoNovel() const { return autoStartSoNovel_; }
    void setAutoStartSoNovel(bool on) { autoStartSoNovel_ = on; save(); }

    static bool isBookFile(const std::string& path);
    static std::vector<std::string> scanDir(const std::string& dir, int maxDepth = 4);

private:
    std::string configFilePath;
    std::vector<RecentFile> recentFiles;
    std::vector<std::string> libraryDirs;
    std::string lastCursorPath;
    int lastCursorOffset_ = 0;
    int sortMode_ = 0;
    bool autoUpdateBooks_ = true;
    bool autoUpdateDownloader_ = true;
    bool autoStartSoNovel_ = true;
    std::string soNovelHome_;

    // 文件路径 → 番茄 book_id 映射
    std::vector<std::pair<std::string, std::string>> bookSources;
};

#endif
