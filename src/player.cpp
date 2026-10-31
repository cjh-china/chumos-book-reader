#include "player.h"
#include "fs_util.h"
#include "http_client.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

#ifdef _WIN32
#include <windows.h>
#endif

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// ==================== Impl ====================

struct Player::Impl {
    ma_device device;
    ma_decoder decoder;
    bool deviceInited = false;
    bool decoderInited = false;

    // 播放游标（PCM 帧）
    ma_uint64 totalFrames = 0;
    ma_uint64 cursorFrames = 0;
    ma_uint32 sampleRate = 44100;
    ma_uint32 channels = 2;

    bool seeking = false;
    ma_uint64 seekTarget = 0;

    std::mt19937 rng{std::random_device{}()};
};

// ==================== 音频回调 ====================

namespace {

void dataCallback(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frameCount) {
    Player::Impl* impl = (Player::Impl*)dev->pUserData;
    if (!impl || !impl->decoderInited) {
        ma_silence_pcm_frames(out, frameCount, dev->playback.format, dev->playback.channels);
        return;
    }

    if (impl->seeking) {
        ma_decoder_seek_to_pcm_frame(&impl->decoder, impl->seekTarget);
        impl->cursorFrames = impl->seekTarget;
        impl->seeking = false;
    }

    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(&impl->decoder, out, frameCount, &got);

    if (got < frameCount) {
        // 尾部未读满，补静音
        ma_uint32 bpf = ma_get_bytes_per_frame(dev->playback.format, dev->playback.channels);
        ma_silence_pcm_frames((ma_uint8*)out + got * bpf, frameCount - got,
                              dev->playback.format, dev->playback.channels);
    }

    impl->cursorFrames += got;
}

} // namespace

// ==================== Player ====================

Player::Player() {
    impl = new Impl();
}

Player::~Player() {
    shutdown();
    delete impl;
    impl = nullptr;
}

bool Player::init() {
    if (!impl) return false;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 0;   // 跟随解码器
    cfg.dataCallback = dataCallback;
    cfg.pUserData = impl;

    if (ma_device_init(nullptr, &cfg, &impl->device) != MA_SUCCESS) {
        errMsg = "音频设备初始化失败";
        return false;
    }
    impl->deviceInited = true;
    return true;
}

void Player::shutdown() {
    if (!impl) return;
    stop();
    if (impl->deviceInited) {
        ma_device_uninit(&impl->device);
        impl->deviceInited = false;
    }
}

bool Player::startDecoding(const Song& s) {
    if (!impl) return false;

    // 关闭旧解码器
    if (impl->decoderInited) {
        ma_decoder_uninit(&impl->decoder);
        impl->decoderInited = false;
    }

    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 2, 0);
    ma_result r;
#ifdef _WIN32
    // miniaudio 的 ma_decoder_init_file 在 Windows 下用 ANSI 路径，
    // 中文路径会失败。改用宽字符版本。
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s.path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) { errMsg = "路径编码转换失败"; return false; }
        std::wstring wp((size_t)wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.path.c_str(), -1, &wp[0], wlen);
        if (!wp.empty() && wp.back() == L'\0') wp.pop_back();
        r = ma_decoder_init_file_w(wp.c_str(), &dcfg, &impl->decoder);
    }
#else
    r = ma_decoder_init_file(s.path.c_str(), &dcfg, &impl->decoder);
#endif

    if (r != MA_SUCCESS) {
        errMsg = "无法打开音频文件: " + fsutil::filename(s.path);
        return false;
    }
    impl->decoderInited = true;

    ma_uint32 sr = 0, ch = 0;
    ma_decoder_get_data_format(&impl->decoder, nullptr, &ch, &sr, nullptr, 0);
    impl->sampleRate = sr ? sr : 44100;
    impl->channels = ch ? ch : 2;
    ma_decoder_get_length_in_pcm_frames(&impl->decoder, &impl->totalFrames);
    impl->cursorFrames = 0;
    impl->seeking = false;

    // 设备采样率需要匹配解码器，不匹配时重建设备
    if (impl->deviceInited && impl->device.sampleRate != impl->sampleRate) {
        ma_device_uninit(&impl->device);
        impl->deviceInited = false;

        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = impl->channels;
        cfg.sampleRate = impl->sampleRate;
        cfg.dataCallback = dataCallback;
        cfg.pUserData = impl;

        if (ma_device_init(nullptr, &cfg, &impl->device) == MA_SUCCESS) {
            impl->deviceInited = true;
        } else {
            errMsg = "音频设备重建失败";
            return false;
        }
    }

    ma_device_set_master_volume(&impl->device, vol);
    errMsg.clear();
    return true;
}

bool Player::play(int index) {
    if (index < 0 || index >= (int)pl.size()) return false;
    cur = index;

    if (!startDecoding(pl[cur])) return false;

    if (ma_device_start(&impl->device) != MA_SUCCESS) {
        errMsg = "无法启动音频输出";
        return false;
    }

    playing = true;
    paused = false;

    if (mode == PlayMode::Shuffle) {
        shuffleHistory.push_back(cur);
        if (shuffleHistory.size() > 200) shuffleHistory.erase(shuffleHistory.begin());
    }
    return true;
}

bool Player::playCurrent() {
    if (cur < 0 || cur >= (int)pl.size()) return false;
    if (impl->decoderInited) {
        ma_decoder_seek_to_pcm_frame(&impl->decoder, 0);
        impl->cursorFrames = 0;
        if (!playing) {
            ma_device_start(&impl->device);
            playing = true;
        }
        paused = false;
        return true;
    }
    return play(cur);
}

void Player::pause() {
    if (!impl || !playing) return;
    ma_device_stop(&impl->device);
    paused = true;
}

void Player::resume() {
    if (!impl || !playing || !paused) return;
    ma_device_start(&impl->device);
    paused = false;
}

void Player::togglePause() {
    if (paused) resume();
    else if (playing) pause();
    else playCurrent();
}

void Player::stop() {
    if (!impl) return;
    if (impl->deviceInited) ma_device_stop(&impl->device);
    if (impl->decoderInited) {
        ma_decoder_uninit(&impl->decoder);
        impl->decoderInited = false;
    }
    playing = false;
    paused = false;
    impl->cursorFrames = 0;
    impl->totalFrames = 0;
}

const Song* Player::current() const {
    if (cur < 0 || cur >= (int)pl.size()) return nullptr;
    return &pl[cur];
}

void Player::setPlaylist(const std::vector<Song>& songs, int startIndex) {
    pl = songs;
    cur = -1;
    shuffleHistory.clear();
    if (!pl.empty() && startIndex >= 0 && startIndex < (int)pl.size()) {
        cur = startIndex;
    }
}

double Player::positionSec() const {
    if (!impl || impl->sampleRate == 0) return 0;
    return (double)impl->cursorFrames / (double)impl->sampleRate;
}

double Player::durationSec() const {
    if (!impl || impl->sampleRate == 0 || impl->totalFrames == 0) {
        const Song* s = current();
        return s ? s->durationSec : 0;
    }
    return (double)impl->totalFrames / (double)impl->sampleRate;
}

void Player::seekTo(double sec) {
    if (!impl || !impl->decoderInited) return;
    if (sec < 0) sec = 0;
    double dur = durationSec();
    if (dur > 0 && sec > dur) sec = dur;

    ma_uint64 target = (ma_uint64)(sec * impl->sampleRate);
    impl->seekTarget = target;
    impl->seeking = true;
}

void Player::setVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    vol = v;
    if (impl && impl->deviceInited) {
        ma_device_set_master_volume(&impl->device, vol);
    }
}

const char* Player::modeName(PlayMode m) {
    switch (m) {
        case PlayMode::Sequential: return "列表循环";
        case PlayMode::Shuffle: return "随机播放";
        case PlayMode::RepeatOne: return "单曲循环";
    }
    return "未知";
}

int Player::pickNextIndex(bool forward) const {
    if (pl.empty()) return -1;
    int n = (int)pl.size();

    if (mode == PlayMode::RepeatOne) return cur;

    if (mode == PlayMode::Shuffle) {
        if (n == 1) return 0;
        // 随机但避免立即重复
        std::uniform_int_distribution<int> dist(0, n - 1);
        int idx = cur;
        for (int attempt = 0; attempt < 10 && idx == cur; attempt++) {
            idx = dist(const_cast<std::mt19937&>(impl->rng));
        }
        return idx;
    }

    // 顺序播放
    if (forward) return (cur + 1) % n;
    return (cur - 1 + n) % n;
}

void Player::next() {
    if (pl.empty()) return;
    int idx = pickNextIndex(true);
    if (idx >= 0) play(idx);
}

void Player::prev() {
    if (pl.empty()) return;
    if (mode == PlayMode::Shuffle && !shuffleHistory.empty()) {
        shuffleHistory.pop_back();
        if (!shuffleHistory.empty()) {
            play(shuffleHistory.back());
            return;
        }
    }
    int idx = pickNextIndex(false);
    if (idx >= 0) play(idx);
}

void Player::tick() {
    // 睡眠定时：到点就停
    if (sleepMode_ == SleepMode::Minutes && sleepDeadlineMs > 0) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now >= sleepDeadlineMs) {
            stop();
            sleepFired = true;
            sleepMode_ = SleepMode::Off;
            sleepDeadlineMs = 0;
            return;
        }
    }

    if (!playing || paused || !impl || !impl->decoderInited) return;
    if (impl->totalFrames == 0) return;

    // 已到末尾（留 2 帧容差）
    if (impl->cursorFrames + 2 >= impl->totalFrames) {
        if (sleepMode_ == SleepMode::EndOfTrack) {
            stop();
            sleepFired = true;
            sleepMode_ = SleepMode::Off;
            return;
        }
        next();
    }
}

// ==================== 睡眠定时 ====================

void Player::setSleepTimer(int minutes) {
    if (minutes <= 0) {
        sleepMode_ = SleepMode::Off;
        sleepDeadlineMs = 0;
        return;
    }
    sleepMode_ = SleepMode::Minutes;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sleepDeadlineMs = now + (long long)minutes * 60 * 1000;
}

void Player::setSleepEndOfTrack(bool on) {
    if (on) {
        sleepMode_ = SleepMode::EndOfTrack;
        sleepDeadlineMs = 0;
    } else if (sleepMode_ == SleepMode::EndOfTrack) {
        sleepMode_ = SleepMode::Off;
    }
}

int Player::sleepRemainSec() const {
    if (sleepMode_ != SleepMode::Minutes || sleepDeadlineMs <= 0) return -1;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long left = sleepDeadlineMs - now;
    if (left <= 0) return 0;
    return (int)(left / 1000);
}

// ==================== 远程播放 ====================

bool Player::downloadToFile(const std::string& url, const std::string& outPath) {
    dlProgress = 0.0;
    dlActive = true;

    HttpClient client;
    client.setTimeout(180);   // 大文件给足时间

    std::map<std::string, std::string> headers;
    headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    std::string data = client.get(url, headers);

    dlActive = false;
    dlProgress = -1.0;

    if (data.empty()) {
        errMsg = "下载失败（空响应）";
        return false;
    }

    fsutil::createDirs(fsutil::parent(outPath));
    if (!fsutil::writeAll(outPath, data)) {
        errMsg = "缓存写入失败";
        return false;
    }
    return true;
}

bool Player::playUrl(const std::string& url, const std::string& title,
                     const std::string& artist, const std::string& cacheDir) {
    if (url.empty()) { errMsg = "URL 为空"; return false; }
    if (!impl || !impl->deviceInited) { errMsg = "音频设备未初始化"; return false; }

    // 缓存文件名：用 URL 的哈希，避免重名
    std::hash<std::string> hasher;
    size_t hv = hasher(url);
    char name[64];
    std::snprintf(name, sizeof(name), "stream_%zx.mp3", hv);

    std::string dir = cacheDir;
    if (dir.empty()) {
        // 默认放在 exe 同级的 cache/ 下
        std::string base = fsutil::exeDir();
        if (base.empty()) base = ".";
        dir = fsutil::join(base, "cache");
    }
    std::string cachePath = fsutil::join(dir, name);

    // 已有缓存且非空则直接用
    if (!(fsutil::isFile(cachePath) && fsutil::fileSize(cachePath) > 4096)) {
        if (!downloadToFile(url, cachePath)) return false;
    }

    // 网易云对无版权歌曲返回约 107KB 的占位音频，需识别并拒绝
    long long sz = fsutil::fileSize(cachePath);
    if (sz < 200000) {
        fsutil::removeFile(cachePath);   // 删掉，避免下次误用
        errMsg = "该曲目无版权（占位音频），请换一条结果";
        return false;
    }

    // 构造一个临时 Song 交给解码器
    Song s;
    s.path = cachePath;
    s.title = title.empty() ? "在线音乐" : title;
    s.artist = artist.empty() ? "未知艺术家" : artist;

    // 清空列表，只播这一首（避免 next 时切到无关曲目）
    pl.clear();
    pl.push_back(s);
    cur = 0;

    if (!startDecoding(pl[0])) return false;

    if (ma_device_start(&impl->device) != MA_SUCCESS) {
        errMsg = "无法启动音频输出";
        return false;
    }

    playing = true;
    paused = false;
    remoteCachePath = cachePath;
    errMsg.clear();
    return true;
}

double Player::downloadProgress() const {
    return dlProgress.load();
}

bool Player::isDownloading() const {
    return dlActive.load();
}
