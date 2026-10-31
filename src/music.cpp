#include "music.h"
#include "fs_util.h"
#include "encoding.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// 读取大端/小端整数
unsigned int readBE32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

// ID3v2 的 syncsafe 整数：每字节只用低 7 位
unsigned int readSyncSafe32(const unsigned char* p) {
    return ((unsigned int)(p[0] & 0x7F) << 21) |
           ((unsigned int)(p[1] & 0x7F) << 14) |
           ((unsigned int)(p[2] & 0x7F) << 7) |
           (unsigned int)(p[3] & 0x7F);
}
unsigned int readBE24(const unsigned char* p) {
    return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | (unsigned int)p[2];
}
unsigned int readLE32(const unsigned char* p) {
    return ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[1] << 8) | (unsigned int)p[0];
}

// 去掉 ID3v2 文本帧开头的编码字节与 BOM
std::string decodeId3Text(const unsigned char* data, size_t size) {
    if (size == 0) return "";
    unsigned char enc = data[0];
    const unsigned char* p = data + 1;
    size_t n = size - 1;

    if (enc == 0) {
        // ISO-8859-1，当作 Latin-1 处理
        return std::string((const char*)p, n);
    }
    if (enc == 3) {
        // UTF-8
        return std::string((const char*)p, n);
    }
    if (enc == 1 || enc == 2) {
        // UTF-16，enc=1 有 BOM，enc=2 无 BOM（默认 LE）
        bool be = false;
        if (enc == 1 && n >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) { be = false; p += 2; n -= 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
        }
        std::string out;
        for (size_t i = 0; i + 1 < n; i += 2) {
            unsigned int cp = be ? ((p[i] << 8) | p[i + 1]) : ((p[i + 1] << 8) | p[i]);
            if (cp == 0) break;
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }
        return out;
    }
    return std::string((const char*)p, n);
}

// 解析 ID3v2 标签（返回 true 表示解析到至少一个字段）
bool parseId3v2(const std::string& data, Song& song) {
    if (data.size() < 10) return false;
    const unsigned char* p = (const unsigned char*)data.data();
    if (std::memcmp(p, "ID3", 3) != 0) return false;

    unsigned char majorVer = p[3];
    unsigned char flags = p[5];

    // tag size 是 syncsafe 整数
    unsigned int tagSize = readSyncSafe32(p + 6);
    size_t end = std::min<size_t>(data.size(), 10 + (size_t)tagSize);

    size_t pos = 10;

    // 跳过扩展头（ID3v2.3/2.4 的 flag bit 6）
    if (flags & 0x40) {
        if (pos + 4 <= end) {
            if (majorVer >= 4) {
                unsigned int extSize = readSyncSafe32(p + pos);
                pos += extSize;
            } else {
                unsigned int extSize = readBE32(p + pos);
                pos += 4 + extSize;
            }
        }
    }

    bool got = false;
    while (pos + 10 <= end) {
        const unsigned char* frame = p + pos;
        if (frame[0] == 0) break;   // 进入填充区

        char id[5] = {0};
        std::memcpy(id, frame, 4);
        if (!std::isalnum((unsigned char)id[0])) break;

        // v2.4 的帧长度也是 syncsafe；v2.2 及更早是普通大端
        unsigned int fsize = (majorVer >= 4) ? readSyncSafe32(frame + 4)
                                             : readBE32(frame + 4);
        if (fsize == 0) break;
        if (pos + 10 + (size_t)fsize > end) break;

        const unsigned char* fdata = frame + 10;
        std::string fid(id);

        if (fid == "TIT2") { song.title = decodeId3Text(fdata, fsize); got = true; }
        else if (fid == "TPE1") { song.artist = decodeId3Text(fdata, fsize); got = true; }
        else if (fid == "TALB") { song.album = decodeId3Text(fdata, fsize); got = true; }

        pos += 10 + (size_t)fsize;
    }

    // 去掉标签值里可能残留的尾部空字符
    auto trimNul = [](std::string& s) {
        while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    };
    trimNul(song.title);
    trimNul(song.artist);
    trimNul(song.album);

    return got;
}

// FLAC Vorbis Comment
bool parseFlacTags(const std::string& data, Song& song) {
    if (data.size() < 4 || std::memcmp(data.data(), "fLaC", 4) != 0) return false;

    size_t pos = 4;
    bool got = false;
    while (pos + 4 <= data.size()) {
        unsigned char h = (unsigned char)data[pos];
        bool last = (h & 0x80) != 0;
        unsigned int type = h & 0x7F;
        unsigned int len = readBE24((const unsigned char*)data.data() + pos + 1);
        pos += 4;
        if (pos + len > data.size()) break;

        if (type == 4) {  // VORBIS_COMMENT
            const unsigned char* p = (const unsigned char*)data.data() + pos;
            size_t n = len;
            if (n >= 4) {
                unsigned int vlen = readLE32(p);
                p += 4; n -= 4;
                if (vlen <= n) { p += vlen; n -= vlen; }   // 跳过 vendor
                if (n >= 4) {
                    unsigned int count = readLE32(p);
                    p += 4; n -= 4;
                    for (unsigned int i = 0; i < count && n >= 4; i++) {
                        unsigned int clen = readLE32(p);
                        p += 4; n -= 4;
                        if (clen > n) break;
                        std::string entry((const char*)p, clen);
                        p += clen; n -= clen;

                        size_t eq = entry.find('=');
                        if (eq != std::string::npos) {
                            std::string key = toLowerAscii(entry.substr(0, eq));
                            std::string val = entry.substr(eq + 1);
                            if (key == "title") { song.title = val; got = true; }
                            else if (key == "artist") { song.artist = val; got = true; }
                            else if (key == "album") { song.album = val; got = true; }
                        }
                    }
                }
            }
        }

        pos += len;
        if (last) break;
    }
    return got;
}

} // namespace

// ==================== LRC ====================

int LyricDoc::indexAt(double t) const {
    if (lines.empty()) return -1;
    int lo = 0, hi = (int)lines.size() - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (lines[mid].timeSec <= t + 0.0001) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

namespace lrc {

LyricDoc parse(const std::string& text) {
    LyricDoc doc;
    std::vector<LyricLine> tmp;
    double offset = 0.0;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // 收集行首所有 [..] 标签
        std::vector<double> times;
        size_t i = 0;
        while (i < line.size() && line[i] == '[') {
            size_t close = line.find(']', i);
            if (close == std::string::npos) break;
            std::string tag = line.substr(i + 1, close - i - 1);
            i = close + 1;

            size_t colon = tag.find(':');
            if (colon == std::string::npos) continue;

            // 判断是否为时间标签：冒号前全为数字，冒号后以数字开头
            bool isTime = (colon > 0);
            for (size_t k = 0; k < colon && isTime; k++) {
                if (!std::isdigit((unsigned char)tag[k])) isTime = false;
            }
            if (isTime) {
                if (colon + 1 >= tag.size() || !std::isdigit((unsigned char)tag[colon + 1])) {
                    isTime = false;
                }
            }

            if (isTime) {
                std::string mm = tag.substr(0, colon);
                std::string rest = tag.substr(colon + 1);
                double sec = std::atof(rest.c_str());
                int minutes = std::atoi(mm.c_str());
                if (minutes >= 0 && sec >= 0) {
                    times.push_back(minutes * 60.0 + sec);
                }
                continue;
            }

            // 元信息：[offset:+500] 等
            std::string key = toLowerAscii(tag.substr(0, colon));
            std::string val = tag.substr(colon + 1);
            if (key == "offset") {
                offset = std::atof(val.c_str()) / 1000.0;
            }
            continue;
        }

        if (times.empty()) continue;

        std::string content = trim(line.substr(i));
        for (double t : times) {
            LyricLine ll;
            ll.timeSec = t;
            ll.text = content;
            tmp.push_back(ll);
        }
    }

    // 应用 offset 并排序
    for (auto& l : tmp) l.timeSec += offset;
    std::stable_sort(tmp.begin(), tmp.end(),
                     [](const LyricLine& a, const LyricLine& b) {
                         return a.timeSec < b.timeSec;
                     });

    doc.lines = std::move(tmp);
    doc.valid = !doc.lines.empty();
    return doc;
}

LyricDoc parseFile(const std::string& path) {
    std::string raw;
    if (!fsutil::readAll(path, raw)) return LyricDoc();
    return parse(encoding::toUtf8(raw));
}

} // namespace lrc

// ==================== 曲库 ====================

namespace musiclib {

bool isAudioFile(const std::string& path) {
    std::string ext = fsutil::extension(path);
    return ext == "mp3" || ext == "flac" || ext == "wav" ||
           ext == "ogg" || ext == "m4a" || ext == "aac";
}

Song readMetadata(const std::string& path) {
    Song song;
    song.path = path;
    song.fileSize = fsutil::fileSize(path);
    song.mtimeMs = fsutil::lastWriteMs(path);

    // 只读文件头（ID3 通常远小于 2MB，含封面时可能较大）
    std::string data;
    fsutil::readHead(path, 2 * 1024 * 1024, data);

    bool got = false;
    if (!data.empty()) {
        if (parseId3v2(data, song)) got = true;
        else if (parseFlacTags(data, song)) got = true;
    }

    // 兜底：文件名
    if (song.title.empty()) song.title = fsutil::stem(path);
    if (song.artist.empty()) song.artist = "未知艺术家";
    if (song.album.empty()) song.album = "";

    if (song.title.empty()) song.title = fsutil::filename(path);
    return song;
}

void attachSidecarFiles(Song& song) {
    std::string base = fsutil::stem(song.path);
    std::string dir = fsutil::parent(song.path);

    // 歌词
    if (song.lyricPath.empty()) {
        for (const char* ext : {".lrc", ".LRC"}) {
            std::string p = fsutil::join(dir, base + ext);
            if (fsutil::isFile(p)) { song.lyricPath = p; break; }
        }
    }

    // 封面
    if (song.coverPath.empty()) {
        for (const char* ext : {".jpg", ".jpeg", ".png", ".JPG", ".PNG"}) {
            std::string p = fsutil::join(dir, base + ext);
            if (fsutil::isFile(p)) { song.coverPath = p; break; }
        }
    }
}

std::vector<Song> scanDir(const std::string& dir, int maxDepth) {
    std::vector<Song> out;
    if (!fsutil::isDirectory(dir)) return out;

    auto all = fsutil::listFilesRecursive(dir, maxDepth);
    for (const auto& f : all) {
        if (!isAudioFile(f)) continue;
        Song s = readMetadata(f);
        attachSidecarFiles(s);
        out.push_back(std::move(s));
    }

    std::sort(out.begin(), out.end(), [](const Song& a, const Song& b) {
        if (a.title != b.title) return a.title < b.title;
        return a.path < b.path;
    });
    return out;
}

} // namespace musiclib

namespace {

// 判断 UTF-8 字符串是否纯 ASCII
bool isAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}

// UTF-8 → UTF-16LE（不含 BOM）
std::string utf8ToUtf16le(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        unsigned int cp = 0;
        int len = 1;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }

        if (i + len > s.size()) break;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;

        if (cp < 0x10000) {
            out += (char)(cp & 0xFF);
            out += (char)((cp >> 8) & 0xFF);
        } else {
            // 代理对
            cp -= 0x10000;
            unsigned int hi = 0xD800 + (cp >> 10);
            unsigned int lo = 0xDC00 + (cp & 0x3FF);
            out += (char)(hi & 0xFF); out += (char)((hi >> 8) & 0xFF);
            out += (char)(lo & 0xFF); out += (char)((lo >> 8) & 0xFF);
        }
    }
    return out;
}

// 构造 USLT（歌词）帧：编码 + 语言 + 内容描述 + 歌词
std::string makeUsltFrame(const std::string& lyrics) {
    std::string payload;
    payload += (char)0x01;          // UTF-16 with BOM
    payload += "chi";               // 语言
    payload += (char)0xFF;          // 内容描述（空串，也要 BOM）
    payload += (char)0xFE;
    payload += (char)0x00;
    payload += (char)0x00;          // 描述结束
    payload += (char)0xFF;          // 歌词正文的 BOM
    payload += (char)0xFE;
    payload += utf8ToUtf16le(lyrics);

    std::string frame;
    frame.append("USLT", 4);
    unsigned int sz = (unsigned int)payload.size();
    frame += (char)((sz >> 24) & 0xFF);
    frame += (char)((sz >> 16) & 0xFF);
    frame += (char)((sz >> 8) & 0xFF);
    frame += (char)(sz & 0xFF);
    frame += (char)0;
    frame += (char)0;
    frame += payload;
    return frame;
}

// 构造 APIC（封面）帧：编码 + MIME + 类型 + 描述 + 图片数据
// 封面是二进制，统一用编码 0（ISO-8859-1）描述字段
std::string makeApicFrame(const std::string& mime,
                          const std::string& image,
                          unsigned char picType = 0x03) {   // 0x03 = 封面正面
    std::string payload;
    payload += (char)0x00;          // 编码 0：描述字段按 Latin-1
    payload += mime;
    payload += (char)0x00;          // MIME 结束
    payload += (char)picType;
    payload += (char)0x00;          // 空描述，直接结束
    payload += image;

    std::string frame;
    frame.append("APIC", 4);
    unsigned int sz = (unsigned int)payload.size();
    frame += (char)((sz >> 24) & 0xFF);
    frame += (char)((sz >> 16) & 0xFF);
    frame += (char)((sz >> 8) & 0xFF);
    frame += (char)(sz & 0xFF);
    frame += (char)0;
    frame += (char)0;
    frame += payload;
    return frame;
}

// 构造一个文本帧（ID3v2.3，编码 1 = UTF-16 with BOM）
std::string makeTextFrame(const char* id, const std::string& text) {
    std::string payload;
    payload += (char)0x01;          // 编码
    payload += (char)0xFF;          // BOM
    payload += (char)0xFE;
    payload += utf8ToUtf16le(text);

    std::string frame;
    frame.append(id, 4);

    // v2.3 帧长度：普通大端 4 字节
    unsigned int sz = (unsigned int)payload.size();
    frame += (char)((sz >> 24) & 0xFF);
    frame += (char)((sz >> 16) & 0xFF);
    frame += (char)((sz >> 8) & 0xFF);
    frame += (char)(sz & 0xFF);

    frame += (char)0;   // flags
    frame += (char)0;
    frame += payload;
    return frame;
}

// 写 syncsafe 4 字节
void writeSyncSafe(std::string& out, unsigned int v) {
    out += (char)((v >> 21) & 0x7F);
    out += (char)((v >> 14) & 0x7F);
    out += (char)((v >> 7) & 0x7F);
    out += (char)(v & 0x7F);
}

// 从 ID3v2 标签里提取所有帧（保留原始字节）
struct RawFrame {
    std::string id;
    std::string data;   // 完整帧（含 10 字节头）
};

bool parseFrames(const std::string& file, size_t& audioStart,
                 std::vector<RawFrame>& frames) {
    frames.clear();
    audioStart = 0;

    if (file.size() < 10) return false;
    const unsigned char* p = (const unsigned char*)file.data();
    if (std::memcmp(p, "ID3", 3) != 0) return false;

    unsigned char major = p[3];
    unsigned char flags = p[5];
    unsigned int tagSize = ((unsigned int)(p[6] & 0x7F) << 21) |
                           ((unsigned int)(p[7] & 0x7F) << 14) |
                           ((unsigned int)(p[8] & 0x7F) << 7) |
                           (unsigned int)(p[9] & 0x7F);
    size_t end = 10 + (size_t)tagSize;
    if (end > file.size()) end = file.size();
    audioStart = end;

    size_t pos = 10;
    // 扩展头
    if (flags & 0x40) {
        if (pos + 4 <= end) {
            unsigned int ext = (major >= 4)
                ? (((unsigned int)(p[pos] & 0x7F) << 21) | ((unsigned int)(p[pos+1] & 0x7F) << 14) |
                   ((unsigned int)(p[pos+2] & 0x7F) << 7) | (unsigned int)(p[pos+3] & 0x7F))
                : ((unsigned int)p[pos] << 24 | (unsigned int)p[pos+1] << 16 |
                   (unsigned int)p[pos+2] << 8 | (unsigned int)p[pos+3]);
            pos += (major >= 4) ? ext : (4 + ext);
        }
    }

    while (pos + 10 <= end) {
        if (p[pos] == 0) break;
        char id[5] = {0};
        std::memcpy(id, p + pos, 4);
        bool valid = true;
        for (int k = 0; k < 4; k++) if (!std::isalnum((unsigned char)id[k])) valid = false;
        if (!valid) break;

        unsigned int fsize = (major >= 4)
            ? (((unsigned int)(p[pos+4] & 0x7F) << 21) | ((unsigned int)(p[pos+5] & 0x7F) << 14) |
               ((unsigned int)(p[pos+6] & 0x7F) << 7) | (unsigned int)(p[pos+7] & 0x7F))
            : ((unsigned int)p[pos+4] << 24 | (unsigned int)p[pos+5] << 16 |
               (unsigned int)p[pos+6] << 8 | (unsigned int)p[pos+7]);

        if (fsize == 0 || pos + 10 + fsize > end) break;

        RawFrame f;
        f.id = id;
        f.data = file.substr(pos, 10 + fsize);
        frames.push_back(std::move(f));

        pos += 10 + fsize;
    }
    return true;
}

} // namespace

namespace {

// 用 newFrames 替换掉 dropIds 指定的帧，其余帧原样保留，重写整个 ID3v2 标签。
bool rebuildId3(const std::string& path,
                const std::vector<RawFrame>& newFrames,
                const std::vector<std::string>& dropIds,
                std::string& error) {
    std::string ext = fsutil::extension(path);
    if (ext != "mp3") {
        error = "目前只支持 MP3 标签写入（FLAC 待实现）";
        return false;
    }

    std::string file;
    if (!fsutil::readAll(path, file)) {
        error = "无法读取文件";
        return false;
    }

    size_t audioStart = 0;
    std::vector<RawFrame> frames;
    parseFrames(file, audioStart, frames);

    // 移除被替换的旧帧
    frames.erase(std::remove_if(frames.begin(), frames.end(),
        [&dropIds](const RawFrame& f) {
            return std::find(dropIds.begin(), dropIds.end(), f.id) != dropIds.end();
        }), frames.end());

    // 新帧放最前面（常见做法）
    std::vector<RawFrame> finalFrames;
    finalFrames.insert(finalFrames.end(), newFrames.begin(), newFrames.end());
    finalFrames.insert(finalFrames.end(), frames.begin(), frames.end());

    // 计算总大小
    size_t bodySize = 0;
    for (const auto& f : finalFrames) bodySize += f.data.size();

    // 加 1KB padding，方便以后修改
    const size_t PADDING = 1024;

    if (bodySize + PADDING > 0x0FFFFFFF) {
        error = "标签过大（超过 ID3v2 上限）";
        return false;
    }

    std::string tag;
    tag += "ID3";
    tag += (char)3;      // v2.3
    tag += (char)0;      // revision
    tag += (char)0;      // flags
    writeSyncSafe(tag, (unsigned int)(bodySize + PADDING));

    for (const auto& f : finalFrames) tag += f.data;
    tag.append(PADDING, '\0');

    // 组装：新标签 + 原音频
    std::string result;
    result.reserve(tag.size() + (file.size() - audioStart));
    result += tag;
    result.append(file, audioStart, file.size() - audioStart);

    // 先写临时文件，再替换原文件（避免中途失败丢数据）
    std::string tmp = path + ".tagtmp";
    if (!fsutil::writeAll(tmp, result)) {
        error = "写入临时文件失败";
        return false;
    }

    if (!fsutil::removeFile(path)) {
        fsutil::removeFile(tmp);
        error = "无法替换原文件（可能被占用）";
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        fsutil::removeFile(tmp);
        error = "重命名失败: " + ec.message();
        return false;
    }

    return true;
}

} // namespace

bool musiclib::writeTags(const std::string& path,
                         const std::string& title,
                         const std::string& artist,
                         const std::string& album,
                         std::string& error) {
    error.clear();

    std::vector<RawFrame> newFrames;
    if (!title.empty()) newFrames.push_back({"TIT2", makeTextFrame("TIT2", title)});
    if (!artist.empty()) newFrames.push_back({"TPE1", makeTextFrame("TPE1", artist)});
    if (!album.empty()) newFrames.push_back({"TALB", makeTextFrame("TALB", album)});

    return rebuildId3(path, newFrames, {"TIT2", "TPE1", "TALB"}, error);
}

bool musiclib::embedMetadata(const std::string& path,
                             const std::string& lyrics,
                             const std::string& coverData,
                             const std::string& coverMime,
                             std::string& error) {
    error.clear();

    if (lyrics.empty() && coverData.empty()) {
        error = "没有可写入的元数据";
        return false;
    }

    std::vector<RawFrame> newFrames;
    std::vector<std::string> dropIds;

    if (!lyrics.empty()) {
        newFrames.push_back({"USLT", makeUsltFrame(lyrics)});
        dropIds.push_back("USLT");
        dropIds.push_back("ULT");    // v2.2 旧标识，一并清掉避免重复
    }

    if (!coverData.empty()) {
        std::string mime = coverMime;
        if (mime.empty()) {
            // 按文件头猜 MIME
            const unsigned char* d = (const unsigned char*)coverData.data();
            if (coverData.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
                mime = "image/jpeg";
            } else if (coverData.size() >= 8 &&
                       d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
                mime = "image/png";
            } else {
                mime = "image/jpeg";
            }
        }
        newFrames.push_back({"APIC", makeApicFrame(mime, coverData)});
        dropIds.push_back("APIC");
        dropIds.push_back("PIC");
    }

    return rebuildId3(path, newFrames, dropIds, error);
}
