#include "netease_api.h"
#include "http_client.h"
#include "mini_json.h"

#include <openssl/evp.h>
#include <openssl/md5.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

namespace netease {

namespace {

const char* kEapiKey = "e82ckenh8dichen8";
const char* kApiHost = "https://interface.music.163.com";
const char* kApiUserAgent =
    "NeteaseMusic 9.0.90/5038 (iPhone; iOS 16.2; zh_CN)";

std::string md5Hex(const std::string& s) {
    unsigned char out[16];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, s.data(), s.size());
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);

    static const char* hex = "0123456789abcdef";
    std::string r;
    r.reserve(32);
    for (unsigned int i = 0; i < len; i++) {
        r += hex[out[i] >> 4];
        r += hex[out[i] & 0x0F];
    }
    return r;
}

std::string aesEcbHex(const std::string& plain, const std::string& key) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                       (const unsigned char*)key.data(), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    std::string cipher;
    cipher.resize(plain.size() + 16);
    int c1 = 0, c2 = 0;
    EVP_EncryptUpdate(ctx, (unsigned char*)&cipher[0], &c1,
                      (const unsigned char*)plain.data(), (int)plain.size());
    EVP_EncryptFinal_ex(ctx, (unsigned char*)&cipher[0] + c1, &c2);
    cipher.resize((size_t)(c1 + c2));
    EVP_CIPHER_CTX_free(ctx);

    static const char* hexU = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(cipher.size() * 2);
    for (unsigned char b : cipher) {
        hex += hexU[b >> 4];
        hex += hexU[b & 0x0F];
    }
    return hex;
}

std::string randomId(int len) {
    static const char* cs =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::mt19937 rng((unsigned)std::time(nullptr) ^ (unsigned)len);
    std::string out;
    out.reserve(len);
    for (int i = 0; i < len; i++) out += cs[rng() % 62];
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// URL 编码（用于 cookie 值）
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

} // namespace

EapiClient::EapiClient() {
    userAgent_ = kApiUserAgent;
}

std::string EapiClient::encryptParams(const std::string& uri, const std::string& jsonText) {
    std::string message = "nobody" + uri + "use" + jsonText + "md5forencrypt";
    std::string digest = md5Hex(message);
    std::string data = uri + "-36cd479b6b5-" + jsonText + "-36cd479b6b5-" + digest;
    return aesEcbHex(data, kEapiKey);
}

std::string EapiClient::buildHeaderJson(const std::string& deviceId,
                                         const std::string& buildver,
                                         const std::string& requestId) const {
    return "{\"osver\":\"16.2\",\"deviceId\":\"" + deviceId +
           "\",\"os\":\"iPhone OS\",\"appver\":\"9.0.90\","
           "\"versioncode\":\"140\",\"mobilename\":\"\","
           "\"buildver\":\"" + buildver + "\","
           "\"resolution\":\"1920x1080\",\"__csrf\":\"\","
           "\"channel\":\"distribution\",\"requestId\":\"" + requestId + "\","
           "\"MUSIC_A\":\"" + cookie_ + "\"}";
}

std::string EapiClient::buildCookieHeader() const {
    // 保持与 header 一致。deviceId/requestId 每次请求都会变，
    // 这里用固定值也能工作（服务端只校验关键字段）。
    std::string buildver = std::to_string((long long)std::time(nullptr)).substr(0, 10);
    std::string c =
        "osver=16.2"
        "; deviceId=anonymous"
        "; os=iPhone%20OS"
        "; appver=9.0.90"
        "; versioncode=140"
        "; mobilename="
        "; buildver=" + buildver +
        "; resolution=1920x1080"
        "; __csrf="
        "; channel=distribution"
        "; MUSIC_A=" + cookie_;
    return c;
}

std::string EapiClient::request(const std::string& uri, const std::string& jsonText, int timeoutSec) {
    err_.clear();

    std::string shortUri = uri;
    if (shortUri.rfind("/api/", 0) == 0) shortUri = shortUri.substr(4);
    std::string url = std::string(kApiHost) + "/eapi" + shortUri;

    std::string buildver = std::to_string((long long)std::time(nullptr)).substr(0, 10);
    const std::string deviceId = randomId(32);
    long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
    char rid[48];
    std::snprintf(rid, sizeof(rid), "%lld_%04d", ms, (int)(std::rand() % 10000));
    const std::string requestId = rid;

    std::string headerJson = buildHeaderJson(deviceId, buildver, requestId);

    std::string payload = jsonText;
    if (payload.size() >= 2 && payload.back() == '}') {
        payload.pop_back();
        if (payload.size() > 1) payload += ",";
        payload += "\"header\":" + headerJson + ",\"e_r\":false}";
    } else {
        payload = "{\"header\":" + headerJson + ",\"e_r\":false}";
    }

    std::string params = encryptParams(uri, payload);
    std::string body = "params=" + params;

    std::string cookie = buildCookieHeader();

    HttpClient client;
    client.setTimeout(timeoutSec);

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";
    headers["User-Agent"] = userAgent_;
    headers["Cookie"] = cookie;
    headers["Referer"] = "https://music.163.com";

    std::string resp = client.post(url, body, headers);
    if (resp.empty()) {
        err_ = "网络请求失败";
        if (!client.lastError().empty()) err_ += "：" + client.lastError();
        return "";
    }
    return resp;
}

bool EapiClient::parseSongs(const std::string& resp, std::vector<Song>& out) {
    minijson::Value root = minijson::parse(resp);
    const minijson::Value& songs = root["songs"];
    const minijson::Value& data = root["data"];
    const minijson::Value& playlist = root["playlist"]["tracks"];

    const minijson::Value* list = nullptr;
    if (songs.size() > 0) list = &songs;
    else if (playlist.size() > 0) list = &playlist;
    else if (data.size() > 0) list = &data;
    else return false;

    for (size_t i = 0; i < list->size(); i++) {
        const minijson::Value& it = (*list)[i];
        Song s;
        s.id = it["id"].toStringValue();
        if (s.id.empty()) continue;
        s.name = it["name"].toStringValue();
        if (s.name.empty()) s.name = it["title"].toStringValue();
        if (s.name.empty()) continue;

        const minijson::Value& artists = it["ar"].size() > 0 ? it["ar"] : it["artists"];
        std::string artist;
        for (size_t k = 0; k < artists.size(); k++) {
            std::string a = artists[k]["name"].toStringValue();
            if (a.empty()) continue;
            if (!artist.empty()) artist += " / ";
            artist += a;
        }
        s.artist = artist.empty() ? "未知艺术家" : artist;

        const minijson::Value& album = it["al"].size() > 0 ? it["al"] : it["album"];
        s.album = album["name"].toStringValue();
        s.coverUrl = album["picUrl"].toStringValue();

        double dur = it["duration"].asNumber();
        if (dur <= 0) dur = it["dt"].asNumber();
        s.durationSec = dur / 1000.0;

        out.push_back(std::move(s));
    }
    return !out.empty();
}

bool EapiClient::search(const std::string& keyword, int limit, std::vector<Song>& out) {
    out.clear();
    if (keyword.empty()) { err_ = "关键词为空"; return false; }

    std::string json = "{\"s\":\"" + jsonEscape(keyword) +
                       "\",\"type\":1,\"limit\":" + std::to_string(limit) +
                       ",\"offset\":0,\"total\":true}";
    std::string resp = request("/api/search/get", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "搜索返回错误 code=" + std::to_string((int)root["code"].asNumber());
        return false;
    }

    const minijson::Value& songs = root["result"]["songs"];
    for (size_t i = 0; i < songs.size(); i++) {
        const minijson::Value& it = songs[i];
        Song s;
        s.id = it["id"].toStringValue();
        if (s.id.empty()) continue;
        s.name = it["name"].toStringValue();
        if (s.name.empty()) continue;

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
        s.coverUrl = it["album"]["picUrl"].toStringValue();
        out.push_back(std::move(s));
    }
    return true;
}

bool EapiClient::songUrl(const std::string& songId, int br, SongUrl& out) {
    out = SongUrl();
    if (songId.empty()) { err_ = "songId 为空"; return false; }

    std::string json = "{\"ids\":\"[\\\"" + jsonEscape(songId) +
                       "\\\"]\",\"br\":" + std::to_string(br) + "}";
    std::string resp = request("/api/song/enhance/player/url", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "获取播放地址失败";
        return false;
    }
    const minijson::Value& data = root["data"];
    if (data.size() == 0) { err_ = "无播放数据"; return false; }

    out.url = data[0]["url"].toStringValue();
    out.br = (int)data[0]["br"].asNumber();
    out.size = (long long)data[0]["size"].asNumber();
    out.type = data[0]["type"].toStringValue();
    if (out.url.empty()) { err_ = "该歌曲无版权或需登录"; return false; }
    return true;
}

bool EapiClient::songUrlEx(const std::string& songId, const std::string& level, SongUrl& out) {
    out = SongUrl();
    if (songId.empty()) { err_ = "songId 为空"; return false; }

    std::string lv = level.empty() ? "exhigh" : level;
    std::string json = "{\"ids\":\"[\\\"" + jsonEscape(songId) +
                       "\\\"]\",\"level\":\"" + jsonEscape(lv) + "\",\"encodeType\":\"flac\"}";
    std::string resp = request("/api/song/enhance/player/url/v1", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "获取播放地址失败 code=" + std::to_string((int)root["code"].asNumber());
        return false;
    }
    const minijson::Value& data = root["data"];
    if (data.size() == 0) { err_ = "无播放数据"; return false; }

    out.url = data[0]["url"].toStringValue();
    out.br = (int)data[0]["br"].asNumber();
    out.size = (long long)data[0]["size"].asNumber();
    out.type = data[0]["type"].toStringValue();
    if (out.url.empty()) { err_ = "该歌曲无版权或需登录"; return false; }
    return true;
}

bool EapiClient::lyric(const std::string& songId, std::string& outLrc) {
    LyricInfo info;
    if (!lyricEx(songId, info)) return false;
    outLrc = info.lrc;
    return true;
}

bool EapiClient::lyricEx(const std::string& songId, LyricInfo& out) {
    out = LyricInfo();
    if (songId.empty()) { err_ = "songId 为空"; return false; }

    std::string json = "{\"id\":\"" + jsonEscape(songId) +
                       "\",\"lv\":-1,\"tv\":-1,\"rv\":-1}";
    std::string resp = request("/api/song/lyric", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) { err_ = "获取歌词失败"; return false; }

    out.lrc = root["lrc"]["lyric"].toStringValue();
    out.translated = root["tlyric"]["lyric"].toStringValue();

    if (out.lrc.empty()) { err_ = "无歌词"; return false; }
    return true;
}

bool EapiClient::userPlaylists(const std::string& userId, std::vector<Playlist>& out) {
    out.clear();
    if (userId.empty()) { err_ = "userId 为空"; return false; }

    std::string json = "{\"uid\":\"" + jsonEscape(userId) +
                       "\",\"limit\":100,\"offset\":0,\"includeVideo\":true}";
    std::string resp = request("/api/user/playlist", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "获取歌单失败 code=" + std::to_string((int)root["code"].asNumber());
        return false;
    }

    const minijson::Value& list = root["playlist"];
    for (size_t i = 0; i < list.size(); i++) {
        Playlist p;
        p.id = list[i]["id"].toStringValue();
        p.name = list[i]["name"].toStringValue();
        p.trackCount = (int)list[i]["trackCount"].asNumber();
        p.coverUrl = list[i]["coverImgUrl"].toStringValue();
        p.creator = list[i]["creator"]["nickname"].toStringValue();
        if (!p.id.empty() && !p.name.empty()) out.push_back(std::move(p));
    }
    return !out.empty();
}

bool EapiClient::playlistDetail(const std::string& playlistId, std::vector<Song>& out) {
    out.clear();
    if (playlistId.empty()) { err_ = "playlistId 为空"; return false; }

    std::string json = "{\"id\":\"" + jsonEscape(playlistId) +
                       "\",\"n\":1000,\"s\":0}";
    std::string resp = request("/api/v6/playlist/detail", json, 20);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "获取歌单详情失败 code=" + std::to_string((int)root["code"].asNumber());
        return false;
    }

    return parseSongs(resp, out);
}

bool EapiClient::toplists(std::vector<Playlist>& out) {
    out.clear();

    std::string json = "{}";
    std::string resp = request("/api/toplist", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "获取排行榜失败 code=" + std::to_string((int)root["code"].asNumber());
        return false;
    }

    const minijson::Value& list = root["list"];
    for (size_t i = 0; i < list.size(); i++) {
        Playlist p;
        p.id = list[i]["id"].toStringValue();
        p.name = list[i]["name"].toStringValue();
        p.trackCount = (int)list[i]["trackCount"].asNumber();
        p.coverUrl = list[i]["coverImgUrl"].toStringValue();
        p.isToplist = true;
        if (!p.id.empty() && !p.name.empty()) out.push_back(std::move(p));
    }
    return !out.empty();
}

bool EapiClient::recommendSongs(std::vector<Song>& out) {
    out.clear();
    if (cookie_.empty()) { err_ = "需要登录才能获取每日推荐"; return false; }

    std::string json = "{\"afresh\":false}";
    std::string resp = request("/api/v1/discovery/recommend/songs", json, 20);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "获取每日推荐失败 code=" + std::to_string((int)root["code"].asNumber());
        return false;
    }

    return parseSongs(resp, out);
}

bool EapiClient::qrKey(QrLoginState& out) {
    out = QrLoginState();

    // 生成唯一 key
    std::string key = std::to_string(std::time(nullptr)) + "_" + randomId(16);

    std::string json = "{\"type\":1}";
    std::string resp = request("/api/login/qrcode/unikey", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "生成二维码 key 失败 code=" + std::to_string((int)root["code"].asNumber());
        return false;
    }
    std::string unikey = root["unikey"].toStringValue();
    if (unikey.empty()) { err_ = "unikey 为空"; return false; }

    out.key = unikey;
    out.url = "https://music.163.com/login?codekey=" + unikey;
    out.code = 801;
    out.message = "等待扫码";
    return true;
}

bool EapiClient::qrCheck(const std::string& key, QrLoginState& out) {
    out = QrLoginState();
    out.key = key;
    if (key.empty()) { err_ = "key 为空"; return false; }

    std::string json = "{\"key\":\"" + jsonEscape(key) + "\",\"type\":1}";
    std::string resp = request("/api/login/qrcode/client/login", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    int code = (int)root["code"].asNumber();
    out.code = code;
    out.message = root["message"].toStringValue();

    if (code == 803) {
        // 登录成功，提取 cookie
        std::string c = root["cookie"].toStringValue();
        if (!c.empty()) {
            // 从完整 cookie 串里只保留关键 token 字段
            std::string keep;
            const char* keys[] = {"MUSIC_A=", "MUSIC_U=", "__csrf=", "NMTID="};
            for (const char* k : keys) {
                size_t p = c.find(k);
                if (p != std::string::npos) {
                    size_t end = c.find(';', p);
                    if (end == std::string::npos) end = c.size();
                    if (!keep.empty()) keep += "; ";
                    keep += c.substr(p, end - p);
                }
            }
            if (keep.empty()) keep = c;
            cookie_ = keep;
        }
        out.message = "登录成功";
    } else if (code == 800) {
        out.message = "二维码已过期";
    } else if (code == 802) {
        out.message = "已扫码，等待手机确认";
    } else {
        out.message = "等待扫码";
    }
    return true;
}

bool EapiClient::loginStatus(UserInfo& out) {
    out = UserInfo();

    std::string json = "{}";
    std::string resp = request("/api/w/nuser/account/get", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) {
        err_ = "查询登录状态失败";
        return false;
    }

    const minijson::Value& profile = root["profile"];
    if (profile.type != minijson::Value::Object) {
        return true;  // 未登录，返回 true 但 valid=false
    }

    out.userId = profile["userId"].toStringValue();
    out.nickname = profile["nickname"].toStringValue();
    out.avatarUrl = profile["avatarUrl"].toStringValue();
    out.valid = !out.userId.empty();

    const minijson::Value& account = root["account"];
    int vipType = (int)account["vipType"].asNumber();
    out.vip = vipType > 0;

    return true;
}

} // namespace netease
