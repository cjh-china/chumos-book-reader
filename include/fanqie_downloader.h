#ifndef FANQIE_DOWNLOADER_H
#define FANQIE_DOWNLOADER_H

#include <string>
#include <vector>

class FanqieDownloader {
public:
    struct BookInfo {
        std::string book_id;
        std::string book_name;
        std::string author;
        std::string score;
        std::string abstract;
        int chapter_count = 0;
    };

    struct ChapterInfo {
        std::string item_id;
        std::string title;
    };

    // 书籍元信息（用于生成下载文件的头部）
    struct BookMeta {
        std::string name;
        std::string author;
        std::string category;
        std::string tags;
        std::string description;
        int chapterCount = 0;
        long long wordCount = 0;
        std::string score;
        bool finished = false;
        bool valid = false;
    };

    explicit FanqieDownloader(int concurrency = 4);
    ~FanqieDownloader();

    // 确保本地 Tomato 服务在运行（未启动则自动拉起），成功返回 true
    bool ensureService();

    std::vector<BookInfo> searchBooks(const std::string& keyword);
    std::vector<ChapterInfo> getChapterList(const std::string& bookId);

    // 获取书籍元信息（书名/作者/简介等）
    BookMeta fetchBookMeta(const std::string& bookId);

    // 生成元信息头部文本（供下载文件使用）
    static std::string formatMetaHeader(const BookMeta& meta);

    // 通过本地服务下载全书并合并成纯文本（供 TUI 阅读）
    std::string buildBookText(const std::string& bookId, int maxChapters = -1);

    // 下载全书，导出为 txt 到 saveDir
    bool downloadBook(const std::string& bookId, const std::string& bookName,
                      const std::string& saveDir, int maxChapters = -1);

    // 配置服务数据目录（工具目录）
    void setToolDir(const std::string& dir) { toolDir = dir; }

    // 下载保存目录（exe 同级 downloads/）
    const std::string& saveDir() const { return defaultSaveDir; }

    // 把书名清洗成合法文件名（替换非法字符）
    static std::string sanitizeName(const std::string& name);

    // 更新已下载的书（增量）。返回新增章节数；-1 表示失败。
    // 会重新拉取目录，只下载缺失的章节，然后重新合并导出 txt。
    int updateBook(const std::string& bookId, const std::string& bookName,
                   const std::string& saveDir);

    // 检查下载器自身是否有新版本（调用 exe --self-update）
    // dryRun=true 时只检查不更新。返回 true 表示已执行（不代表有新版本）。
    bool selfUpdate(bool autoConfirm, std::string& output);

    // 下载器当前版本号（如 "2.4.16"）；失败返回空
    std::string downloaderVersion();

private:
    std::string httpGet(const std::string& path, const std::string& query);
    std::string httpPostJson(const std::string& path, const std::string& jsonBody);
    bool isServiceUp();
    bool spawnService();
    std::string locateExe();
    std::string readChaptersJsonl(const std::string& bookId, int maxChapters);
    static std::string urlEncode(const std::string& s);
    static std::string stripHtml(const std::string& html);

    std::string baseUrl;
    std::string toolDir;
    std::string defaultSaveDir;
    int concurrency;
    bool serviceChecked;
};

#endif
