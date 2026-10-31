#ifndef SONOVEL_H
#define SONOVEL_H

#include <string>
#include <vector>

// so-novel（https://github.com/freeok/so-novel）本地 WebUI 客户端。
// 需要用户自行下载并运行 sonovel.exe（默认监听 127.0.0.1:7765）。
class SoNovel {
public:
    struct Book {
        int sourceId = 0;
        std::string sourceName;
        std::string url;
        std::string bookName;
        std::string author;
        std::string category;
        std::string latestChapter;
        std::string lastUpdateTime;
        std::string status;
    };

    struct Source {
        int id = 0;
        std::string name;
        std::string url;
        std::string comment;
        bool disabled = false;
    };

    // 服务地址，默认 127.0.0.1:7765，可用 SONOVEL_API_BASE 环境变量覆盖
    SoNovel();

    // 探测服务是否在线
    bool isAvailable();

    // 确保服务可用：未运行则尝试自动拉起 sonovel.exe
    // homeDir 为空则自动探测常见位置
    bool ensureService(const std::string& homeDir = "", int waitSeconds = 20);

    // 查找 sonovel.exe 所在目录；找不到返回空
    static std::string findHome(const std::string& hint = "");

    // 聚合搜索（会并发查询所有书源，较慢，可能 10~30 秒）
    std::vector<Book> search(const std::string& keyword);

    // 下载到服务端 downloads 目录。成功返回文件路径（服务端路径），失败返回空。
    // format 为空则用服务端配置（通常 txt）
    std::string download(const Book& book, const std::string& format = "txt");

    // 列出已下载书籍的文件名
    std::vector<std::string> listDownloaded();

    // 把服务端 downloads 里的文件复制到本地目录
    bool fetchToLocal(const std::string& fileName, const std::string& localDir,
                      std::string& outPath);

    const std::string& lastError() const { return errMsg; }

    static std::string baseUrl();
    // so-novel 的下载目录（服务端视角，本地就是 exe 同目录 downloads）
    static std::string serverDownloadDir();

private:
    std::string errMsg;
    std::string httpGet(const std::string& path, int timeoutSec);
};

#endif
