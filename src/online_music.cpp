#include "online_music.h"
#include "http_client.h"
#include "mini_json.h"
#include "fs_util.h"

#include <algorithm>
#include <cctype>

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

std::string httpGet(const std::string& url, int timeout = 25) {
    HttpClient client;
    client.setTimeout(timeout);
    std::map<std::string, std::string> headers;
    headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    headers["Accept"] = "application/json";
    return client.get(url, headers);
}

} // namespace

std::vector<std::string> OnlineMusicService::availableSources() {
    std::vector<std::string> list;
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
    if (keyword.empty()) return all;

    std::vector<std::string> order = sources;
    if (order.empty()) order = availableSources();

    int limit = 20;
    for (const auto& src : order) {
        std::vector<OnlineSong> part;
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

std::vector<OnlineSong> OnlineMusicService::searchAudius(const std::string& kw, int limit) {
    std::vector<OnlineSong> out;

    std::string url = "https://api.audius.co/v1/tracks/search?query=" +
                      urlEncode(kw) + "&app_name=ebook_reader";
    std::string body = httpGet(url);
    if (body.empty()) {
        if (errMsg.empty()) errMsg = "Audius 请求失败";
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
    std::string body = httpGet(url);
    if (body.empty()) {
        if (errMsg.empty()) errMsg = "iTunes 请求失败";
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

#ifdef HAS_LOCAL_MUSIC_API
    // 本地音源：搜索结果里没拿到地址时，播放前再解析一次
    if (song.source == "netease-local") {
        std::string u;
        if (resolveViaLocalApi(song.id, u)) return u;
    }
#endif

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
        if (errMsg.empty()) errMsg = "myfreemp3 请求失败";
        return out;
    }

    minijson::Value root = minijson::parse(body);
    if (root["code"].asNumber() != 200) {
        if (errMsg.empty()) errMsg = "myfreemp3 返回错误";
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

bool OnlineMusicService::isLocalApiAvailable() {
    // 0 = 未探测, 1 = 在线, -1 = 离线（本次运行不再重试）
    static int state = 0;
    if (state != 0) return state > 0;

    HttpClient c;
    c.setTimeout(2);   // 缩短到 2 秒，降低首次等待感
    std::string r = c.get(localApiBase() + "/search?keywords=test&limit=1");
    bool ok = !r.empty() && r.find("\"result\"") != std::string::npos;
    state = ok ? 1 : -1;
    return ok;
}

bool OnlineMusicService::resolveViaLocalApi(const std::string& songId, std::string& outUrl) {
    outUrl.clear();
    if (songId.empty()) return false;

    std::string url = localApiBase() + "/song/url?id=" + urlEncode(songId) + "&br=320000";
    std::string body = httpGet(url, 15);
    if (body.empty()) return false;

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
    std::string body = httpGet(url, 15);
    if (body.empty()) return false;

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
    std::string body = httpGet(url, 15);
    if (body.empty()) {
        if (errMsg.empty()) errMsg = "本地音乐服务未响应";
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
        error = "无法获取播放地址";
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
            error = "下载失败（空响应）";
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

    // 歌词
    {
        std::string lrcText = song.lyricText;

        // 本地音源不带歌词，单独请求一次
        if (lrcText.empty()) {
#ifdef HAS_LOCAL_MUSIC_API
            if (song.source == "netease-local") {
                std::string got;
                if (fetchLocalLyric(song.id, got)) lrcText = got;
            }
#endif
        }

        if (!lrcText.empty()) {
            std::string lrcPath = fsutil::join(dir, baseName + ".lrc");
            if (!fsutil::isFile(lrcPath)) {
                fsutil::writeAll(lrcPath, lrcText);
            }
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
                fsutil::writeAll(coverPath, img);
            }
        }
    }

    return audioPath;
}
