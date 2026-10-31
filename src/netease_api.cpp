#include "netease_api.h"
#include "http_client.h"
#include "mini_json.h"

#include <openssl/evp.h>
#include <openssl/md5.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
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

// AES-128-ECB + PKCS7，返回大写 hex
std::string aesEcbHex(const std::string& plain, const std::string& key) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                       (const unsigned char*)key.data(), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx, 1);

    // 按块处理，防止超长输入溢出栈缓冲
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

// 极简 JSON 字符串转义（用于拼接请求体）
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

std::string EapiClient::request(const std::string& uri, const std::string& jsonText, int timeoutSec) {
    err_.clear();

    // eapi 短 URI：/api/search/get -> /eapi/search/get
    std::string shortUri = uri;
    if (shortUri.rfind("/api/", 0) == 0) shortUri = shortUri.substr(4);  // 保留开头斜杠
    std::string url = std::string(kApiHost) + "/eapi" + shortUri;

    // 匿名设备信息（对应 Node 版 request.js 的 header 对象）
    // buildver：10 位时间戳前缀（与 Node 版 now().toString().substr(0,10) 一致）
    std::string buildver = std::to_string((long long)std::time(nullptr)).substr(0, 10);
    const std::string deviceId = randomId(32);
    // requestId 格式：<unix 毫秒>_<4 位随机数>（与 Node 版 now() + '_' + padStart(4) 一致）
    long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
    char rid[48];
    std::snprintf(rid, sizeof(rid), "%lld_%04d", ms, (int)(std::rand() % 10000));
    const std::string requestId = rid;

    // 关键：eapi 模式的 header 必须参与加密（放进 data.header），
    // 否则服务端返回 {"msg":"参数错误","code":400}
    std::string headerJson =
        "{\"osver\":\"16.2\",\"deviceId\":\"" + deviceId +
        "\",\"os\":\"iPhone OS\",\"appver\":\"9.0.90\","
        "\"versioncode\":\"140\",\"mobilename\":\"\","
        "\"buildver\":\"" + buildver + "\","
        "\"resolution\":\"1920x1080\",\"__csrf\":\"\","
        "\"channel\":\"distribution\",\"requestId\":\"" + requestId + "\","
        "\"MUSIC_A\":\"" + cookie_ + "\"}";

    // 把 header 与 e_r 注入到业务 JSON 里（与 Node 版 request.js 保持一致）
    // e_r=false 表示响应不加密，服务端返回明文 JSON
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

    // 请求头里的 Cookie 与 header 对象保持一致
    // 关键 1：Cookie 值必须 URL 编码（Node 版 createHeaderCookie 用 encodeURIComponent），
    //         例如 "iPhone OS" -> "iPhone%20OS"。
    // 关键 2：MUSIC_A 必须出现（匿名 token，空值也要发），
    //         否则服务端返回 200 但空 body。
    std::string cookie =
        "osver=16.2"
        "; deviceId=" + deviceId +
        "; os=iPhone%20OS"
        "; appver=9.0.90"
        "; versioncode=140"
        "; mobilename="
        "; buildver=" + buildver +
        "; resolution=1920x1080"
        "; __csrf="
        "; channel=distribution"
        "; requestId=" + requestId +
        "; MUSIC_A=" + cookie_;

    // 调试：设置 EAPI_DUMP=<文件路径> 可导出完整请求，便于与 curl 命令行对照
    if (const char* dump = std::getenv("EAPI_DUMP")) {
        FILE* f = fopen(dump, "wb");
        if (f) {
            fprintf(f, "URL: %s\nBODY: params=%s\nCOOKIE: %s\n",
                    url.c_str(), params.c_str(), cookie.c_str());
            fclose(f);
        }
    }

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

bool EapiClient::lyric(const std::string& songId, std::string& outLrc) {
    outLrc.clear();
    if (songId.empty()) { err_ = "songId 为空"; return false; }

    std::string json = "{\"id\":\"" + jsonEscape(songId) +
                       "\",\"lv\":-1,\"tv\":-1}";
    std::string resp = request("/api/song/lyric", json, 15);
    if (resp.empty()) return false;

    minijson::Value root = minijson::parse(resp);
    if ((int)root["code"].asNumber() != 200) { err_ = "获取歌词失败"; return false; }
    std::string lrc = root["lrc"]["lyric"].toStringValue();
    if (lrc.empty()) { err_ = "无歌词"; return false; }
    outLrc = lrc;
    return true;
}

} // namespace netease
