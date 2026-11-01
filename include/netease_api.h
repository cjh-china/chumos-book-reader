#ifndef NETEASE_API_H
#define NETEASE_API_H

#include <string>
#include <vector>

// 网易云音乐 eapi 客户端（C++ 原生实现，不依赖 Node）。
// 直接调用 https://interface.music.163.com/eapi/*，请求体用 AES-128-ECB 加密。
// 未登录可搜索、播放、歌词；登录后可用高音质、歌单、推荐。
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

struct LyricInfo {
    std::string lrc;         // 原文
    std::string translated;  // 中文翻译（可能为空）
};

struct Playlist {
    std::string id;
    std::string name;
    int trackCount = 0;
    std::string coverUrl;
    std::string creator;
    bool isToplist = false;
};

struct UserInfo {
    std::string userId;
    std::string nickname;
    std::string avatarUrl;
    bool vip = false;
    bool valid = false;
};

struct QrLoginState {
    std::string key;
    std::string url;         // 二维码内容，手机浏览器打开即可授权
    int code = 801;          // 801=待扫码 802=待确认 800=已过期 803=成功
    std::string message;
};

class EapiClient {
public:
    EapiClient();

    // ---- 匿名可用 ----
    bool search(const std::string& keyword, int limit, std::vector<Song>& out);
    bool songUrl(const std::string& songId, int br, SongUrl& out);
    bool lyric(const std::string& songId, std::string& outLrc);

    // ---- 需登录才完整（未登录也能尝试，但可能返回空）----
    // 指定音质级别：standard / higher / exhigh / lossless / hires
    bool songUrlEx(const std::string& songId, const std::string& level, SongUrl& out);
    bool lyricEx(const std::string& songId, LyricInfo& out);
    bool userPlaylists(const std::string& userId, std::vector<Playlist>& out);
    bool playlistDetail(const std::string& playlistId, std::vector<Song>& out);
    bool toplists(std::vector<Playlist>& out);
    bool recommendSongs(std::vector<Song>& out);

    // ---- 登录 ----
    // 生成二维码 key；成功后 out.url 是二维码内容
    bool qrKey(QrLoginState& out);
    // 轮询扫码状态；code=803 时自动把 cookie 存进本对象
    bool qrCheck(const std::string& key, QrLoginState& out);
    // 查询当前登录状态（未登录时 out.valid=false）
    bool loginStatus(UserInfo& out);
    // 清除 cookie
    void logout() { cookie_.clear(); }

    // ---- cookie 管理 ----
    void setCookie(const std::string& c) { cookie_ = c; }
    const std::string& cookie() const { return cookie_; }
    bool loggedIn() const { return !cookie_.empty(); }

    const std::string& lastError() const { return err_; }

    // eapi 加密（公开供测试）
    static std::string encryptParams(const std::string& uri, const std::string& jsonText);

private:
    std::string request(const std::string& uri, const std::string& jsonText, int timeoutSec);
    std::string buildCookieHeader() const;
    std::string buildHeaderJson(const std::string& deviceId,
                                const std::string& buildver,
                                const std::string& requestId) const;
    bool parseSongs(const std::string& resp, std::vector<Song>& out);

    std::string err_;
    std::string cookie_;
    std::string userAgent_;
};

} // namespace netease

#endif
