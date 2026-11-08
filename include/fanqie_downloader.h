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

    // 下载器服务状态（/api/status）
    struct ServerStatus {
        bool ok = false;
        std::string version;
        std::string saveDir;
        std::string bindAddr;
        bool locked = false;   // Web UI 是否启用了密码锁
        bool docker = false;
    };

    // 服务端下载配置（/api/config，只暴露 Web UI 能改的几项）
    struct WebConfig {
        bool valid = false;
        std::string novelFormat = "txt";   // txt / epub / pdf
        bool bulkFiles = false;            // true=分章文件，false=合并单文件
        bool enableAudiobook = false;      // 是否同时生成有声书
        std::string audiobookFormat = "mp3";  // mp3 / wav
        bool askFormat = false;            // 下载前询问格式
    };

    // 预览信息（/api/preview，比 BookMeta 多封面与在读人数）
    struct PreviewInfo {
        bool valid = false;
        std::string bookId;
        std::string name;
        std::string originalName;
        std::string author;
        std::string category;
        std::string description;
        std::string tags;
        std::string score;
        std::string readCountText;
        std::string firstChapter;
        std::string lastChapter;
        std::string coverUrl;    // 服务端相对路径，如 /api/preview-cover/<key>
        int chapterCount = 0;
        long long wordCount = 0;
        bool finished = false;
    };

    // 下载任务快照（/api/jobs?id=N）
    struct JobInfo {
        bool valid = false;
        int id = 0;
        std::string bookId;
        std::string title;
        std::string author;
        std::string state;      // queued / running / done / failed / cancelled
        std::string message;
        std::string savePhase;  // TextSave / AudioSave ...
        int savedChapters = 0;
        int totalChapters = 0;
        std::vector<std::string> formatOptions;     // 非空表示任务在等格式选择
        std::vector<std::string> bookNameOptions;   // 非空表示任务在等书名选择

        bool finished() const {
            return state == "done" || state == "failed" || state == "cancelled";
        }
    };

    // 服务端书库条目（/api/library）
    struct LibraryEntry {
        std::string kind;     // dir / file
        std::string name;
        std::string relPath;
        std::string ext;
        long long size = 0;
    };

    // 追更扫描结果（/api/updates）
    struct UpdateEntry {
        std::string bookId;
        std::string bookName;
        std::string folder;
        int localTotal = 0;
        int remoteTotal = 0;
        int newCount = 0;
        bool hasUpdate = false;
    };

    // 下载历史（/api/history）
    struct HistoryEntry {
        std::string timestamp;
        std::string bookId;
        std::string bookName;
        std::string author;
        std::string status;
        std::string progressText;
        int selectedChapters = 0;
        int successChapters = 0;
        int failedChapters = 0;
    };

    // 新建任务的下载参数
    struct DownloadOptions {
        std::string format;            // 空=沿用服务端配置
        int audiobookMode = -1;        // -1=不变，0=关闭有声书，1=开启
        std::string audiobookFormat;   // 空=沿用服务端配置
        int rangeStart = 0;            // 0=不限制
        int rangeEnd = 0;
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

    // ---------------- 2.4.x 服务端接口 ----------------
    // 服务状态（版本 / 保存目录 / 是否加锁）
    ServerStatus status();

    // 读写服务端下载配置（格式、有声书、是否分章）
    WebConfig webConfig();
    bool applyConfig(const WebConfig& cfg, std::string& err);

    // 书籍预览：封面、章数、字数、在读人数、最新章
    PreviewInfo preview(const std::string& bookId);

    // 创建下载任务（异步），成功返回任务 id，失败返回 0
    int startJob(const std::string& bookId, const DownloadOptions& opt, std::string& err);

    // 查询任务快照
    JobInfo job(int id);

    // 取消任务 / 回应任务里的格式、书名选择
    bool cancelJob(int id);
    bool chooseFormat(int id, const std::string& format);
    bool chooseBookName(int id, const std::string& name);

    // 浏览服务端书库（相对保存目录）。startScan=false 时只读缓存，适合轮询。
    // running / scanned 非空时回填服务端扫描状态。
    std::vector<LibraryEntry> library(const std::string& path = "",
                                      const std::string& name = "",
                                      bool startScan = true,
                                      bool* running = nullptr,
                                      int* scanned = nullptr);

    // 下载服务端文件到内存（/download/<relPath>）
    bool fetchServerFile(const std::string& relPath, std::string& bytes);

    // 取书籍封面 JPEG（/api/preview-cover-by-book/<id>）
    bool fetchCover(const std::string& bookId, std::string& bytes);

    // 把任务产出的文件（txt/epub/pdf）从服务端取回本地目录。
    // 成功返回本地绝对路径；失败返回空并写 err。
    std::string importProduced(const std::string& bookId, const std::string& bookName,
                               const std::string& format, const std::string& localDir,
                               std::string& err, int waitSeconds = 20);

    // 追更扫描。startScan=true 时触发新扫描；返回当前已知结果，running 反映扫描是否进行中。
    std::vector<UpdateEntry> updates(bool startScan, bool& running);

    // 下载历史（最近 limit 条）
    std::vector<HistoryEntry> history(int limit = 50);

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
