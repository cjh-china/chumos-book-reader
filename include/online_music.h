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
    bool previewOnly = false; // iTunes 只有 30 秒试听
};

class OnlineMusicService {
public:
    // 搜索：按音源优先级依次尝试，合并结果
    // sources 为空表示用默认顺序 {"audius", "itunes"}
    std::vector<OnlineSong> search(const std::string& keyword,
                                   const std::vector<std::string>& sources = {});

    // 确保 streamUrl 可用；失败返回空
    std::string resolveStream(const OnlineSong& song);

    // 下载到本地：音频 + 歌词(.lrc) + 封面(.jpg)
    // outDir 为空则用 exe 同级 downloads/music/
    // 返回保存的音频文件路径；失败返回空，error 给出原因
    std::string downloadSong(const OnlineSong& song, const std::string& outDir,
                             std::string& error);

    // 把标题+艺术家拼成合法文件名（不含扩展名）
    static std::string makeFileName(const OnlineSong& song);

    // 默认下载目录（exe 同级 downloads/music）
    static std::string defaultDownloadDir();

    const std::string& lastError() const { return errMsg; }

    // 可用音源列表（供 UI 显示）
    static std::vector<std::string> availableSources();

#ifdef HAS_LOCAL_MUSIC_API
    // 本地 NeteaseCloudMusicApi 服务地址（编译期由 CMake 注入）
    static std::string localApiBase();

    // 查找本机可用的 NeteaseCloudMusicApi 安装目录。
    // 返回 main.js 所在目录；找不到返回空。
    // 查找顺序：NETEASE_API_HOME 环境变量 → npm 全局目录 → exe 同级 tools/ → 常见路径
    static std::string findLocalApiHome();

    // 探测本地服务是否在线；不在线时尝试自动拉起（每个进程只试一次）
    // 首次探测失败后本次运行内不再重试（避免每次都等超时）
    bool isLocalApiAvailable();

    // 用本地服务解析真实播放地址（绕过网易云版权限制）
    bool resolveViaLocalApi(const std::string& songId, std::string& outUrl);

    // 用本地服务获取歌词（LRC 文本）
    bool fetchLocalLyric(const std::string& songId, std::string& outLrc);
#endif

private:
    std::string errMsg;
    std::string lastError_;   // 供 resolveViaLocalApi / fetchLocalLyric 等辅助函数记录原因

    // 原生 eapi 音源（C++ 直连 interface.music.163.com，不需要 Node）
    std::vector<OnlineSong> searchNetease(const std::string& kw, int limit);

    std::vector<OnlineSong> searchAudius(const std::string& kw, int limit);
    std::vector<OnlineSong> searchITunes(const std::string& kw, int limit);
    std::vector<OnlineSong> searchMyFreeMP3(const std::string& kw, int limit);

#ifdef HAS_LOCAL_MUSIC_API
    std::vector<OnlineSong> searchLocalApi(const std::string& kw, int limit);

    // 后台启动 node main.js，最多等 waitSeconds 秒
    static bool startLocalApi(const std::string& home, int waitSeconds);
#endif
};

#endif
