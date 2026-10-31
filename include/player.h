#ifndef PLAYER_H
#define PLAYER_H

#include "music.h"
#include <atomic>
#include <string>
#include <vector>

enum class PlayMode {
    Sequential,   // 列表循环
    Shuffle,      // 随机
    RepeatOne     // 单曲循环
};

class Player {
public:
    Player();
    ~Player();

    // 初始化音频后端，成功返回 true
    bool init();
    void shutdown();

    // 曲库
    void setPlaylist(const std::vector<Song>& songs, int startIndex = 0);
    const std::vector<Song>& playlist() const { return pl; }
    int currentIndex() const { return cur; }
    const Song* current() const;

    // 播放控制
    bool play(int index);

    // 直接播放网络地址（先下载到缓存目录，再本地解码）
    // cacheDir 为空则用系统临时目录
    bool playUrl(const std::string& url, const std::string& title,
                 const std::string& artist, const std::string& cacheDir = "");

    // 下载进度（0.0~1.0，-1 表示未在下载）
    double downloadProgress() const;
    bool isDownloading() const;
    bool playCurrent();
    void pause();
    void resume();
    void togglePause();
    void stop();
    void next();
    void prev();

    // 状态查询
    bool isPlaying() const { return playing; }
    bool isPaused() const { return paused; }
    double positionSec() const;
    double durationSec() const;

    // 跳转（秒）
    void seekTo(double sec);

    // 音量 0.0 ~ 1.0
    void setVolume(float v);
    float volume() const { return vol; }

    // 播放模式
    void setMode(PlayMode m) { mode = m; }
    PlayMode getMode() const { return mode; }
    static const char* modeName(PlayMode m);

    // 每帧调用：检测歌曲是否播完，自动切下一首；检查睡眠定时
    void tick();

    // 睡眠定时
    enum class SleepMode { Off, Minutes, EndOfTrack };
    void setSleepTimer(int minutes);        // 0 表示取消
    void setSleepEndOfTrack(bool on);
    SleepMode sleepMode() const { return sleepMode_; }
    int sleepRemainSec() const;             // 剩余秒数，-1 表示不限时
    bool sleepTriggered() const { return sleepFired; }
    void clearSleepTrigger() { sleepFired = false; }

    // 错误信息（最近一次失败原因）
    const std::string& lastError() const { return errMsg; }

public:
    // 音频后端实现细节（回调需要访问）
    struct Impl;

private:
    std::vector<Song> pl;
    int cur = -1;
    bool playing = false;
    bool paused = false;
    float vol = 0.7f;
    PlayMode mode = PlayMode::Sequential;
    std::string errMsg;

    // 随机播放历史（用于 prev）
    std::vector<int> shuffleHistory;

    Impl* impl = nullptr;

    // 睡眠定时
    SleepMode sleepMode_ = SleepMode::Off;
    long long sleepDeadlineMs = 0;
    bool sleepFired = false;

    // 远程播放：缓存文件与下载状态
    std::string remoteCachePath;
    std::atomic<double> dlProgress{-1.0};
    std::atomic<bool> dlActive{false};

    int pickNextIndex(bool forward) const;
    bool startDecoding(const Song& s);
    bool downloadToFile(const std::string& url, const std::string& outPath);
};

#endif
