#ifndef READER_H
#define READER_H

#include <string>
#include <vector>
#include "tui.h"
#include "config.h"

#ifdef HAS_CURL
#include "fanqie_downloader.h"
#include "sonovel.h"
#endif

struct Book {
    std::string title;
    std::string author;
    std::vector<std::string> content;      // 原始行
    std::vector<std::string> chapters;     // 章节标题
    std::vector<int> chapterStarts;       // 章节起始行号
    std::vector<std::string> wrappedContent; // 自动换行后的行
};

class Reader {
public:
    Reader(TUI* tui);
    ~Reader();

    bool initConfig();
    bool loadBook(const std::string& filename);
    void resetBook();
    // manageTui=false 时由调用方负责 TUI 初始化/关闭（用于返回书库）
    void run(bool manageTui = true);

    // 书库界面：返回选中的文件路径，空字符串表示退出
    std::string showLibrary();

    const Config& config() const { return cfg; }
    bool hasBookInMemory() const { return bookLoadedInMemory; }
    void clearMemoryFlag() { bookLoadedInMemory = false; }
    // 阅读界面按 q/ESC 请求返回书库（而非退出程序）
    bool wantsLibrary() const { return returnToLibrary; }

#ifdef HAS_MUSIC
    // 书库按 M 请求进入音乐模块
    bool wantsMusic() const { return requestMusic; }
    void clearMusicRequest() { requestMusic = false; }
#endif

    // 加载失败时是否走番茄回退（书库场景为 true，命令行直开为 false）
    void setAutoFallback(bool on) { autoFallback = on; }

    // 加载书籍；文件不存在且已开启自动回退时，尝试从番茄下载同名书。
    // 失败时 failReason 给出可展示的原因。
    bool loadBookOrRecover(const std::string& path, std::string& failReason);

#ifdef HAS_CURL
public:
    // 尝试从番茄下载同名书籍。返回下载后的文件路径；失败返回空。
    std::string tryFanqieFallback(const std::string& filePath);
private:
#endif

private:
    void buildWrappedContent();
    void display();
    void handleInput(int key);
    void nextPage();
    void prevPage();
    void scrollUp();
    void scrollDown();
    void goToTop();
    void goToBottom();
    void showTOC();
    void showHelp();
    int getCurrentChapterIndex() const;

    // 搜索相关
    void openSearchMenu();
    void searchInBook();
    void searchFanqie();
    void searchSoNovel();
    void loadTextAsBook(const std::string& text, const std::string& title);

    // 书库相关
    struct LibraryEntry {
        std::string path;
        std::string display;    // 相对路径/文件名
        std::string info;       // 大小 + 时间
        long long size = 0;
        long long mtime = 0;
        std::string nameLower;  // 用于过滤的小写文件名
        bool isHeader = false;
        bool isRecent = false;
    };

    static std::string downloadDir();
    void rebuildLibrary();
    void showSettings();

    // 从文件名提取书名（去掉站点标记、序号、扩展名等）
    static std::string extractBookTitle(const std::string& filePath);
    // 书名归一化（去空白/标点，用于精确比对）
    static std::string normalizeTitle(const std::string& title);


    void addLibraryFolderDialog();
    void removeLibraryFolderDialog();
    void sortLibrary();

    // 返回选中项在 entries 中的下标；-1=取消 -2=刷新 -3=添加 -4=移除
    int openBookPicker(const std::string& title, const std::vector<LibraryEntry>& entries,
                       bool persistCursor);

    static std::string fmtSize(long long bytes);
    static std::string fmtTime(long long ms);

    // 书签与进度
    void saveProgress();
    void toggleBookmark();
    void showBookmarks();
    void showBookmarkMenu();

    TUI* tui;
    Book book;
    int currentLine;
    int linesPerPage;
    bool running;
    int padding;
    std::string lastQuery;
    std::vector<int> matchLines;
    int matchIndex;

    Config cfg;
    std::vector<LibraryEntry> libEntries;
    bool libraryDirty = true;
    bool bookLoadedInMemory = false;   // 番茄搜索后内容已直接载入 book
    bool returnToLibrary = false;      // 阅读中请求返回书库
    bool autoFallback = false;         // 加载失败时自动到番茄找同名书
#ifdef HAS_MUSIC
    bool requestMusic = false;         // 书库请求进入音乐模块
#endif
};

#endif
