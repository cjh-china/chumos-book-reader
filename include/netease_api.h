#ifndef NETEASE_API_H
#define NETEASE_API_H

#include <string>
#include <vector>

// 网易云音乐 eapi 客户端（C++ 原生实现，不依赖 Node）。
// 直接调用 https://interface.music.163.com/eapi/*，请求体用 AES-128-ECB 加密。
// 未登录状态下可用：搜索、歌曲播放地址、歌词。
namespace netease {

struct Song {
    std::string id;
    std::string name;
    std::string artist;      // 多艺术家用 " / " 连接
    std::string album;
    double durationSec = 0;
    std::string coverUrl;
};

struct SongUrl {
    std::string url;
    int br = 0;              // 码率（bit/s）
    long long size = 0;
    std::string type;        // "mp3" / "flac"
};

class EapiClient {
public:
    EapiClient();

    // 搜索单曲；返回 false 时 err 给出原因
    bool search(const std::string& keyword, int limit, std::vector<Song>& out);

    // 获取播放地址（优先 br 指定的码率，拿不到就返回可用的最高码率）
    bool songUrl(const std::string& songId, int br, SongUrl& out);

    // 获取 LRC 歌词（原文，可能为空）
    bool lyric(const std::string& songId, std::string& outLrc);

    const std::string& lastError() const { return err_; }

    // eapi 加密（公开供测试）：返回 params 的十六进制字符串
    static std::string encryptParams(const std::string& uri, const std::string& jsonText);

private:
    // 发一个 eapi 请求，返回响应体（JSON 文本）；失败返回空并设置 err_
    std::string request(const std::string& uri, const std::string& jsonText, int timeoutSec);

    std::string err_;
    std::string cookie_;
    std::string userAgent_;
};

} // namespace netease

#endif
