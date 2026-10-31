#ifndef CONFIG_H
#define CONFIG_H

#include <string>
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

    static bool isBookFile(const std::string& path);
    static std::vector<std::string> scanDir(const std::string& dir, int maxDepth = 4);

private:
    std::string configFilePath;
    std::vector<RecentFile> recentFiles;
    std::vector<std::string> libraryDirs;
    std::string lastCursorPath;
    int lastCursorOffset_ = 0;
    int sortMode_ = 0;
};

#endif
