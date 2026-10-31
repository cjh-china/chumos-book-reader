#include "online_music.h"
#include "http_client.h"
#include "mini_json.h"
#include "fs_util.h"
#ifdef HAS_MUSIC
#include "netease_api.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// URL 编码（query 参数用）
std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}


namespace {

std::string base64Decode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(unsigned char)tbl[i]] = i;

    std::string out;
    out.reserve(in.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = rev[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hexv(in[i + 1]);
            int l = hexv(in[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        if (in[i] == '+') out += ' ';
        else out += in[i];
    }
    return out;
}

} // namespace

// 发 GET 并把失败原因带回去：errDetail 非空时写入具体错误，
// 调用方据此设置 errMsg，避免界面只显示"请求失败"。
std::string httpGet(const std::string& url, std::string& errDetail, int timeout = 25) {
    HttpClient client;
    client.setTimeout(timeout);
    std::map<std::string, std::string> headers;
    headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    headers["Accept"] = "application/json";
    errDetail.clear();
    std::string body = client.get(url, headers);
    if (body.empty()) {
        errDetail = client.lastError();
    }
    return body;
}

} // namespace

std::vector<std::string> OnlineMusicService::availableSources() {
    std::vector<std::string> list;
#ifdef HAS_MUSIC
    // 原生 eapi，优先级最高：不需要 Node，也不受外网音源失效影响
    list.push_back("netease");
#endif
#ifdef HAS_LOCAL_MUSIC_API
    list.push_back("netease-local");
#endif
    list.push_back("myfreemp3");
    list.push_back("audius");
    list.push_back("itunes");
    return list;
}

std::vector<OnlineSong> OnlineMusicService::search(const std::string& keyword,
                                                   const std::vector<std::string>& sources) {
    std::vector<OnlineSong> all;
    errMsg.clear();
    lastError_.clear();
    if (keyword.empty()) return all;

    std::vector<std::string> order = sources;
    if (order.empty()) order = availableSources();

    int limit = 20;
    for (const auto& src : order) {
        std::vector<OnlineSong> part;
#ifdef HAS_MUSIC
        if (src == "netease") {
            part = searchNetease(keyword, limit);
        } else
#endif
#ifdef HAS_LOCAL_MUSIC_API
        if (src == "netease-local") {
            part = searchLocalApi(keyword, limit);
        } else
#endif
        if (src == "myfreemp3") {
            part = searchMyFreeMP3(keyword, limit);
        } else if (src == "audius") {
            part = searchAudius(keyword, limit);
        } else if (src == "itunes") {
            part = searchITunes(keyword, limit);
        }
        all.insert(all.end(), part.begin(), part.end());
    }

    if (all.empty() && errMsg.empty()) errMsg = "所有音源都没有结果";
    return all;
}

#ifdef HAS_MUSIC

std::vector<OnlineSong> OnlineMusicService::searchNetease(const std::string& kw, int limit) {
    std::vector<OnlineSong> out;

    netease::EapiClient client;
    std::vector<netease::Song> songs;
    if (!client.search(kw, limit, songs)) {
        if (errMsg.empty()) errMsg = "网易云搜索失败：" + client.lastError();
        return out;
    }

    for (const auto& s : songs) {
        OnlineSong m;
        m.id = s.id;
        m.title = s.name;
        m.artist = s.artist;
        m.album = s.album;
        m.durationSec = s.durationSec;
        m.source = "netease";
        m.coverUrl = s.coverUrl;

        // 解析播放地址（失败留空，播放时还会再试一次）
        netease::SongUrl u;
        if (client.songUrl(s.id, 320000, u)) {
            m.streamUrl = u.url;
        }

        // 歌词只抓前几条，避免拖慢搜索
        if (out.size() < 8) {
            std::string lrc;
            if (client.lyric(s.id, lrc)) m.lyricText = lrc;
        }

        out.push_back(std::move(m));
    }

    return out;
}

#endif // HAS_MUSIC

std::vector<OnlineSong> OnlineMusicService::searchAudius(const std::string& kw, int limit) {
    std::vector<OnlineSong> out;

    std::string url = "https://api.audius.co/v1/tracks/search?query=" +
                      urlEncode(kw) + "&app_name=ebook_reader";
    std::string detail;
    std::string body = httpGet(url, detail);
    if (body.empty()) {
        if (errMsg.empty()) {
            errMsg = detail.empty() ? "Audius 请求失败" : "Audius 请求失败：" + detail;
        }
        return out;
    }

    minijson::Value root = minijson::parse(body);
    const minijson::Value& data = root["data"];
    for (size_t i = 0; i < data.size() && (int)out.size() < limit; i++) {
        const minijson::Value& t = data[i];

        OnlineSong s;
        s.id = t["id"].toStringValue();
        if (s.id.empty()) continue;

        s.title = t["title"].toStringValue();
        if (s.title.empty()) s.title = "未知曲目";
        s.artist = t["user"]["name"].toStringValue();
        if (s.artist.empty()) s.artist = "未知艺术家";
        s.album = "";
        s.durationSec = t["duration"].asNumber();
        s.source = "audius";

        // stream 地址（需跟随重定向，HttpClient 已开启 FOLLOWLOCATION）
        s.streamUrl = "https://api.audius.co/v1/tracks/" + s.id +
                      "/stream?app_name=ebook_reader";

        // 封面
        std::string art = t["artwork"]["480x480"].toStringValue();
        if (art.empty()) art = t["artwork"]["150x150"].toStringValue();
        s.coverUrl = art;

        out.push_back(std::move(s));
    }

    return out;
}

std::vector<OnlineSong> OnlineMusicService::searchITunes(const std::string& kw, int limit) {
    std::vector<OnlineSong> out;

    std::string url = "https://itunes.apple.com/search?term=" + urlEncode(kw) +
                      "&media=music&limit=" + std::to_string(limit);
    std::string detail;
    std::string body = httpGet(url, detail);
    if (body.empty()) {
        if (errMsg.empty()) {
            errMsg = detail.empty() ? "iTunes 请求失败" : "iTunes 请求失败：" + detail;
        }
        return out;
    }

    minijson::Value root = minijson::parse(body);
    const minijson::Value& results = root["results"];
    for (size_t i = 0; i < results.size(); i++) {
        const minijson::Value& t = results[i];

        std::string preview = t["previewUrl"].toStringValue();
        if (preview.empty()) continue;

        OnlineSong s;
        s.id = t["trackId"].toStringValue();
        if (s.id.empty()) s.id = t["collectionId"].toStringValue();
        s.title = t["trackName"].toStringValue();
        if (s.title.empty()) s.title = "未知曲目";
        s.artist = t["artistName"].toStringValue();
        s.album = t["collectionName"].toStringValue();
        s.durationSec = t["trackTimeMillis"].asNumber() / 1000.0;
        if (s.durationSec <= 0) s.durationSec = 30.0;
        s.source = "itunes";
        s.streamUrl = preview;
        s.coverUrl = t["artworkUrl100"].toStringValue();
        s.previewOnly = true;

        out.push_back(std::move(s));
    }

    return out;
}

std::string OnlineMusicService::resolveStream(const OnlineSong& song) {
    if (!song.streamUrl.empty()) return song.streamUrl;

#ifdef HAS_MUSIC
    // 原生 eapi 音源：搜索结果里没拿到地址时，播放前再解析一次
    if (song.source == "netease") {
        netease::EapiClient client;
        netease::SongUrl u;
        // 依次降级尝试码率，兼容只有低码率的曲目
        if (client.songUrl(song.id, 320000, u) ||
            client.songUrl(song.id, 192000, u) ||
            client.songUrl(song.id, 128000, u)) {
            return u.url;
        }
        lastError_ = "网易云无可用播放地址（" +
                     (client.lastError().empty() ? "可能需要登录或已下架" : client.lastError()) + "）";
    }
#endif

#ifdef HAS_LOCAL_MUSIC_API
    // 本地音源：搜索结果里没拿到地址时，播放前再解析一次
    if (song.source == "netease-local") {
        std::string u;
        if (resolveViaLocalApi(song.id, u)) return u;
        if (!lastError_.empty()) return "";
    }
#endif

    if (lastError_.empty()) {
        lastError_ = "音源 " + song.source + " 没有返回播放地址";
    }
    return "";
}

std::vector<OnlineSong> OnlineMusicService::searchMyFreeMP3(const std::string& kw, int limit) {
    std::vector<OnlineSong> out;

    HttpClient client;
    client.setTimeout(25);

    std::map<std::string, std::string> headers;
    headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    headers["Content-Type"] = "application/x-www-form-urlencoded; charset=UTF-8";
    headers["X-Requested-With"] = "XMLHttpRequest";
    headers["Referer"] = "https://www.myfreemp3.com.cn/";
    headers["Origin"] = "https://www.myfreemp3.com.cn";

    std::string form = "type=netease&filter=name&page=1&input=" + urlEncode(kw);
    std::string body = client.post("https://www.myfreemp3.com.cn/", form, headers);

    if (body.empty()) {
        if (errMsg.empty()) {
            errMsg = client.lastError().empty()
                         ? "myfreemp3 请求失败"
                         : "myfreemp3 请求失败：" + client.lastError();
        }
        return out;
    }

    minijson::Value root = minijson::parse(body);
    if (root["code"].asNumber() != 200) {
        if (errMsg.empty()) {
            errMsg = "myfreemp3 返回错误（code=" +
                     std::to_string((int)root["code"].asNumber()) + "）";
        }
        return out;
    }

    const minijson::Value& list = root["data"]["list"];
    for (size_t i = 0; i < list.size() && (int)out.size() < limit; i++) {
        const minijson::Value& it = list[i];

        OnlineSong s;
        s.id = it["id"].toStringValue();
        if (s.id.empty()) continue;

        s.title = it["title"].toStringValue();
        if (s.title.empty()) continue;
        s.artist = it["author"].toStringValue();
        if (s.artist.empty()) s.artist = "未知艺术家";
        s.album = it["album"].toStringValue();
        s.source = "myfreemp3";

        // 优先 320，退化到 128 或通用 url
        std::string u = it["url_320"].toStringValue();
        if (u.empty()) u = it["url"].toStringValue();
        if (u.empty()) u = it["url_128"].toStringValue();
        if (u.empty()) {
            u = "http://music.163.com/song/media/outer/url?id=" + s.id + ".mp3";
        }
        s.streamUrl = u;

        s.coverUrl = it["pic"].toStringValue();

        // 歌词：data:text/plain,... 或纯文本
        std::string lrc = it["lrc"].toStringValue();
        if (lrc.rfind("data:", 0) == 0) {
            size_t comma = lrc.find(',');
            if (comma != std::string::npos) {
                std::string meta = lrc.substr(0, comma);
                std::string payload = lrc.substr(comma + 1);
                if (meta.find("base64") != std::string::npos) {
                    lrc = base64Decode(payload);
                } else {
                    lrc = urlDecode(payload);
                }
            }
        }
        s.lyricText = lrc;

        out.push_back(std::move(s));
    }

    return out;
}

#ifdef HAS_LOCAL_MUSIC_API

std::string OnlineMusicService::localApiBase() {
#ifdef LOCAL_MUSIC_API_BASE
    return LOCAL_MUSIC_API_BASE;
#else
    return "http://127.0.0.1:3000";
#endif
}

namespace {

// 该目录是不是 NeteaseCloudMusicApi 安装根（有 app.js + node_modules）
// 注意：app.js 是服务入口，main.js 只是模块导出，不能用来启动
bool isApiHome(const std::string& dir) {
    if (dir.empty() || !fsutil::isDirectory(dir)) return false;
    return fsutil::isFile(fsutil::join(dir, "app.js")) &&
           fsutil::isDirectory(fsutil::join(dir, "node_modules"));
}

// 在 appdata 下拼 npm 全局 node_modules 路径
std::string npmGlobalDir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) {
        std::string p = fsutil::join(appdata, "npm");
        p = fsutil::join(p, "node_modules");
        return fsutil::join(p, "NeteaseCloudMusicApi");
    }
#endif
    return "";
}

} // namespace

std::string OnlineMusicService::findLocalApiHome() {
    std::vector<std::string> candidates;

    const char* env = std::getenv("NETEASE_API_HOME");
    if (env && *env) candidates.push_back(env);

    candidates.push_back(npmGlobalDir());

    // exe 同级
    std::string base = fsutil::exeDir();
    if (!base.empty()) {
        candidates.push_back(fsutil::join(base, "NeteaseCloudMusicApi"));
        candidates.push_back(fsutil::join(fsutil::join(base, "tools"), "NeteaseCloudMusicApi"));
    }

    // 从 exe 目录向上找
    std::string cur = base;
    for (int i = 0; i < 4 && !cur.empty(); i++) {
        std::string up = fsutil::parent(cur);
        if (up == cur || up.empty()) break;
        cur = up;
        candidates.push_back(fsutil::join(fsutil::join(cur, "tools"), "NeteaseCloudMusicApi"));
    }

#ifdef _WIN32
    candidates.push_back("D:\\NeteaseCloudMusicApi");
    candidates.push_back("C:\\NeteaseCloudMusicApi");
#endif

    for (const auto& d : candidates) {
        if (isApiHome(d)) return d;
    }
    return "";
}

bool OnlineMusicService::startLocalApi(const std::string& home, int waitSeconds) {
#ifdef _WIN32
    std::string nodeExe = "node.exe";   // 依赖 PATH
    // 优先用 exe 同目录/系统里的 node
    std::string localNode = fsutil::join(fsutil::exeDir(), "node.exe");
    if (fsutil::isFile(localNode)) nodeExe = localNode;

    std::string entry = fsutil::join(home, "app.js");
    std::string cmd = "\"" + nodeExe + "\" \"" + entry + "\"";
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, home.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    std::string cmd = "cd \"" + home + "\" && nohup node main.js >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) return false;
#endif

    // 轮询等待就绪
    int tries = waitSeconds * 2;
    for (int i = 0; i < tries; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        HttpClient c;
        c.setTimeout(2);
        std::string r = c.get(localApiBase() + "/search?keywords=test&limit=1");
        if (!r.empty() && r.find("\"result\"") != std::string::npos) return true;
    }
    return false;
}

bool OnlineMusicService::isLocalApiAvailable() {
    // 0 = 未探测, 1 = 在线, -1 = 离线（本次运行不再重试）
    static int state = 0;
    if (state != 0) return state > 0;

    HttpClient c;
    c.setTimeout(2);   // 缩短到 2 秒，降低首次等待感
    std::string r = c.get(localApiBase() + "/search?keywords=test&limit=1");
    if (!r.empty() && r.find("\"result\"") != std::string::npos) {
        state = 1;
        return true;
    }

    // 不在线 → 尝试自动拉起（本进程只试一次）
    std::string home = findLocalApiHome();
    if (!home.empty() && startLocalApi(home, 15)) {
        state = 1;
        return true;
    }

    state = -1;
    return false;
}

bool OnlineMusicService::resolveViaLocalApi(const std::string& songId, std::string& outUrl) {
    outUrl.clear();
    if (songId.empty()) return false;

    std::string url = localApiBase() + "/song/url?id=" + urlEncode(songId) + "&br=320000";
    std::string detail;
    std::string body = httpGet(url, detail, 15);
    if (body.empty()) {
        lastError_ = detail;
        return false;
    }

    minijson::Value root = minijson::parse(body);
    const minijson::Value& data = root["data"];
    if (data.size() == 0) return false;

    std::string u = data[0]["url"].toStringValue();
    if (u.empty()) return false;
    outUrl = u;
    return true;
}

bool OnlineMusicService::fetchLocalLyric(const std::string& songId, std::string& outLrc) {
    outLrc.clear();
    if (songId.empty()) return false;

    std::string url = localApiBase() + "/lyric?id=" + urlEncode(songId);
    std::string detail;
    std::string body = httpGet(url, detail, 15);
    if (body.empty()) {
        lastError_ = detail;
        return false;
    }

    minijson::Value root = minijson::parse(body);
    if ((int)root["code"].asNumber() != 200) return false;

    std::string lrc = root["lrc"]["lyric"].toStringValue();
    if (lrc.empty()) return false;

    outLrc = lrc;
    return true;
}

std::vector<OnlineSong> OnlineMusicService::searchLocalApi(const std::string& kw, int limit) {
    std::vector<OnlineSong> out;

    // 服务不在线就直接跳过，不再发请求
    if (!isLocalApiAvailable()) return out;

    std::string url = localApiBase() + "/search?keywords=" + urlEncode(kw) +
                      "&limit=" + std::to_string(limit) + "&type=1";
    std::string detail;
    std::string body = httpGet(url, detail, 15);
    if (body.empty()) {
        if (errMsg.empty()) {
            errMsg = detail.empty() ? "本地音乐服务未响应"
                                    : "本地音乐服务未响应：" + detail;
        }
        return out;
    }

    minijson::Value root = minijson::parse(body);
    if ((int)root["code"].asNumber() != 200) {
        if (errMsg.empty()) errMsg = "本地音乐服务返回错误";
        return out;
    }

    const minijson::Value& songs = root["result"]["songs"];
    for (size_t i = 0; i < songs.size() && (int)out.size() < limit; i++) {
        const minijson::Value& it = songs[i];

        OnlineSong s;
        s.id = it["id"].toStringValue();
        if (s.id.empty()) continue;

        s.title = it["name"].toStringValue();
        if (s.title.empty()) continue;

        // 艺术家是数组
        const minijson::Value& artists = it["artists"];
        std::string artist;
        for (size_t k = 0; k < artists.size(); k++) {
            std::string a = artists[k]["name"].toStringValue();
            if (a.empty()) continue;
            if (!artist.empty()) artist += " / ";
            artist += a;
        }
        s.artist = artist.empty() ? "未知艺术家" : artist;
        s.album = it["album"]["name"].toStringValue();
        s.durationSec = it["duration"].asNumber() / 1000.0;
        s.source = "netease-local";

        // 先尝试解析真实地址，失败则留空由播放时再试
        std::string realUrl;
        if (resolveViaLocalApi(s.id, realUrl)) {
            s.streamUrl = realUrl;
        }

        // 封面
        std::string pic = it["album"]["picUrl"].toStringValue();
        if (pic.empty()) pic = it["artists"][0]["img1v1Url"].toStringValue();
        s.coverUrl = pic;

        // 歌词（只对前几条抓，避免拖慢搜索）
        if (out.size() < 8) {
            std::string lrc;
            if (fetchLocalLyric(s.id, lrc)) s.lyricText = lrc;
        }

        out.push_back(std::move(s));
    }

    return out;
}

#endif // HAS_LOCAL_MUSIC_API

// ==================== 下载到本地 ====================

std::string OnlineMusicService::defaultDownloadDir() {
    std::string base = fsutil::exeDir();
    if (base.empty()) base = ".";
    return fsutil::join(fsutil::join(base, "downloads"), "music");
}

std::string OnlineMusicService::makeFileName(const OnlineSong& song) {
    std::string name = song.title;
    if (!song.artist.empty() && song.artist != "未知艺术家") {
        name += " - " + song.artist;
    }

    // 去掉路径非法字符
    const std::string illegal = "\\/:*?\"<>|";
    for (char c : illegal) {
        std::replace(name.begin(), name.end(), c, '_');
    }

    // 去掉尾部空白与点（Windows 不允许）
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }

    if (name.empty()) name = "untitled";

    // 限制长度（按 UTF-8 字符数，避免切坏汉字）
    const size_t MAX_CHARS = 80;
    size_t i = 0, count = 0;
    while (i < name.size() && count < MAX_CHARS) {
        unsigned char c = (unsigned char)name[i];
        int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        i += (size_t)len;
        count++;
    }
    if (i < name.size()) name = name.substr(0, i);

    return name;
}

std::string OnlineMusicService::downloadSong(const OnlineSong& song,
                                             const std::string& outDir,
                                             std::string& error) {
    error.clear();

    std::string url = resolveStream(song);
    if (url.empty()) {
        error = lastError_.empty() ? "无法获取播放地址"
                                   : "无法获取播放地址：" + lastError_;
        return "";
    }

    std::string dir = outDir.empty() ? defaultDownloadDir() : outDir;
    fsutil::createDirs(dir);

    std::string baseName = makeFileName(song);
    std::string audioPath = fsutil::join(dir, baseName + ".mp3");

    // 已存在则跳过（按大小粗判，>100KB 视为有效）
    if (fsutil::isFile(audioPath) && fsutil::fileSize(audioPath) > 100000) {
        // 但仍然补齐歌词/封面（可能上次没下全）
    } else {
        HttpClient client;
        client.setTimeout(180);
        std::map<std::string, std::string> headers;
        headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
        std::string data = client.get(url, headers);

        if (data.empty()) {
            error = client.lastError().empty()
                        ? "下载失败（空响应）"
                        : "下载失败：" + client.lastError();
            return "";
        }
        if (data.size() < 200000) {
            error = "音频过小（可能无版权）";
            return "";
        }

        if (!fsutil::writeAll(audioPath, data)) {
            error = "写入失败";
            return "";
        }
    }

    // 歌词：元数据缺失不算致命错误，但要把原因记下来（界面会显示）
    std::string warn;
    {
        std::string lrcText = song.lyricText;

        if (lrcText.empty()) {
#ifdef HAS_MUSIC
            if (song.source == "netease") {
                netease::EapiClient c;
                std::string got;
                if (c.lyric(song.id, got)) lrcText = got;
                else if (!c.lastError().empty()) warn += "歌词：" + c.lastError() + "; ";
            }
#endif
#ifdef HAS_LOCAL_MUSIC_API
            if (song.source == "netease-local") {
                std::string got;
                if (fetchLocalLyric(song.id, got)) lrcText = got;
                else if (!lastError_.empty()) warn += "歌词：" + lastError_ + "; ";
            }
#endif
        }

        if (!lrcText.empty()) {
            std::string lrcPath = fsutil::join(dir, baseName + ".lrc");
            if (!fsutil::isFile(lrcPath)) {
                if (!fsutil::writeAll(lrcPath, lrcText)) {
                    warn += "歌词写入失败; ";
                }
            }
        } else if (warn.empty()) {
            warn += "该曲目没有歌词; ";
        }
    }

    // 封面
    if (!song.coverUrl.empty()) {
        std::string coverPath = fsutil::join(dir, baseName + ".jpg");
        if (!fsutil::isFile(coverPath)) {
            HttpClient cc;
            cc.setTimeout(30);
            std::map<std::string, std::string> h;
            h["User-Agent"] = "Mozilla/5.0";
            std::string img = cc.get(song.coverUrl, h);
            if (!img.empty() && img.size() > 1000) {
                if (!fsutil::writeAll(coverPath, img)) warn += "封面写入失败; ";
            } else {
                warn += "封面下载失败" +
                        (cc.lastError().empty() ? "" : "：" + cc.lastError()) + "; ";
            }
        }
    } else {
        warn += "该曲目没有封面; ";
    }

    // 下载成功但有元数据缺失时，通过 error 字段把提示带回界面
    if (!warn.empty()) error = warn;

    return audioPath;
}
