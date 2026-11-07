#ifndef ONLINE_MUSIC_H
#define ONLINE_MUSIC_H

#include <string>
#include <vector>

// 在线音乐条目
struct OnlineSong {
    std::string id;           // 音源内唯一 ID
    std::string title;
    std::string artist;
    std::string album;
    double durationSec = 0;
    std::string source;       // "netease" / "netease-local" / "myfreemp3" / "audius" / "itunes"
    std::string streamUrl;    // 直接可播放的地址（可能为空，需 resolve）
    std::string coverUrl;
    std::string lyricText;    // LRC 原文（可能为空）
    std::string transLyric;   // 中文翻译（可能为空）
    bool previewOnly = false; // iTunes 只有 30 秒试听
};

// 网易云歌单（含排行榜）
struct OnlinePlaylist {
    std::string id;
    std::string name;
    int trackCount = 0;
    std::string coverUrl;
    std::string creator;
    bool isToplist = false;
};

// 网易云账号信息
struct NeteaseAccount {
    bool loggedIn = false;
    std::string userId;
    std::string nickname;
    bool vip = false;
};

class OnlineMusicService {
public:
    OnlineMusicService();
    ~OnlineMusicService();

    // 搜索：按音源优先级依次尝试，合并结果
    std::vector<OnlineSong> search(const std::string& keyword,
                                   const std::vector<std::string>& sources = {});

    // 确保 streamUrl 可用；失败返回空
    std::string resolveStream(const OnlineSong& song);

    // 下载到本地：音频 + 歌词(.lrc) + 封面(.jpg)
    std::string downloadSong(const OnlineSong& song, const std::string& outDir,
                             std::string& error);

    static std::string makeFileName(const OnlineSong& song);
    static std::string defaultDownloadDir();

    const std::string& lastError() const { return errMsg; }

    static std::vector<std::string> availableSources();

    // ==================== 网易云账号 / 歌单 / 排行榜 ====================
#ifdef HAS_MUSIC

    // ---- 登录 ----
    // 生成二维码。成功返回 true，qr 里带 key 和 url
    bool beginQrLogin(std::string& qrKey, std::string& qrUrl, std::string& message);
    // 轮询扫码状态。code: 801 待扫 / 802 待确认 / 800 过期 / 803 成功
    bool pollQrLogin(const std::string& qrKey, int& code, std::string& message);
    // 查询登录态
    bool account(NeteaseAccount& out);
    // 退出登录（清 cookie + 删本地文件）
    void logoutNetease();

    // ---- 歌单 ----
    bool neteasePlaylists(std::vector<OnlinePlaylist>& out);
    bool neteasePlaylistSongs(const std::string& playlistId, std::vector<OnlineSong>& out);

    // ---- 排行榜 ----
    bool neteaseToplists(std::vector<OnlinePlaylist>& out);
    bool neteaseToplistSongs(const std::string& toplistId, std::vector<OnlineSong>& out);

    // ---- 每日推荐（需登录）----
    bool neteaseRecommend(std::vector<OnlineSong>& out);

    // ---- 歌词翻译 ----
    bool neteaseLyricEx(const std::string& songId, std::string& lrc, std::string& translated);

    // ---- cookie 持久化 ----
    bool loadNeteaseSession();
    bool saveNeteaseSession();
    const std::string& neteaseCookie() const;
    // 会话文件的实际路径 / 是否已落盘（便于诊断与测试）
    static std::string sessionPath();
    static bool sessionPersisted();

    // 从浏览器复制的完整 Cookie 字符串导入
    bool importNeteaseCookie(const std::string& rawCookie);

    // 根据登录状态选择音质：未登录 exhigh(128k)，登录后 lossless
    std::string preferredLevel() const;
#endif

#ifdef HAS_LOCAL_MUSIC_API
    static std::string localApiBase();
    static std::string findLocalApiHome();
    bool isLocalApiAvailable();
    bool resolveViaLocalApi(const std::string& songId, std::string& outUrl);
    bool fetchLocalLyric(const std::string& songId, std::string& outLrc);
#endif

private:
    std::string errMsg;
    std::string lastError_;

    std::vector<OnlineSong> searchNetease(const std::string& kw, int limit);
    std::vector<OnlineSong> searchAudius(const std::string& kw, int limit);
    std::vector<OnlineSong> searchITunes(const std::string& kw, int limit);
    std::vector<OnlineSong> searchMyFreeMP3(const std::string& kw, int limit);

#ifdef HAS_LOCAL_MUSIC_API
    std::vector<OnlineSong> searchLocalApi(const std::string& kw, int limit);
    static bool startLocalApi(const std::string& home, int waitSeconds);
#endif

};

#endif
