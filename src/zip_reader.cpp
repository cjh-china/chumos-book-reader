#include "zip_reader.h"
#include "fs_util.h"
#include <zlib.h>
#include <cstring>

namespace zip {

namespace {

unsigned int rd16(const std::string& d, size_t p) {
    return (unsigned int)(unsigned char)d[p] | ((unsigned int)(unsigned char)d[p + 1] << 8);
}

unsigned int rd32(const std::string& d, size_t p) {
    return (unsigned int)(unsigned char)d[p]
         | ((unsigned int)(unsigned char)d[p + 1] << 8)
         | ((unsigned int)(unsigned char)d[p + 2] << 16)
         | ((unsigned int)(unsigned char)d[p + 3] << 24);
}

} // namespace

bool Reader::open(const std::string& path) {
    data_.clear();
    entries_.clear();
    err_.clear();

    if (!fsutil::readAll(path, data_)) {
        err_ = "无法读取文件";
        return false;
    }
    if (data_.size() < 22) {
        err_ = "文件太小，不是有效的 ZIP";
        return false;
    }

    // 从尾部向前找 EOCD 签名 0x06054b50
    size_t eocd = std::string::npos;
    size_t limit = data_.size() >= 22 + 65535 ? data_.size() - 22 - 65535 : 0;
    for (size_t p = data_.size() - 22 + 1; p-- > limit;) {
        if (rd32(data_, p) == 0x06054b50u) { eocd = p; break; }
        if (p == 0) break;
    }
    if (eocd == std::string::npos) {
        err_ = "找不到 ZIP 中央目录结尾";
        return false;
    }

    unsigned int total = rd16(data_, eocd + 10);
    unsigned int cdOffset = rd32(data_, eocd + 16);

    if (cdOffset == 0xFFFFFFFFu || total == 0xFFFFu) {
        err_ = "不支持 ZIP64";
        return false;
    }
    if (cdOffset >= data_.size()) {
        err_ = "中央目录偏移非法";
        return false;
    }

    size_t p = cdOffset;
    for (unsigned int i = 0; i < total; i++) {
        if (p + 46 > data_.size() || rd32(data_, p) != 0x02014b50u) {
            err_ = "中央目录条目损坏";
            return false;
        }
        Entry e;
        e.method = (unsigned short)rd16(data_, p + 10);
        e.crc = rd32(data_, p + 16);
        e.compSize = rd32(data_, p + 20);
        e.uncompSize = rd32(data_, p + 24);
        unsigned int nameLen = rd16(data_, p + 28);
        unsigned int extraLen = rd16(data_, p + 30);
        unsigned int commentLen = rd16(data_, p + 32);
        e.localOffset = rd32(data_, p + 42);

        if (p + 46 + nameLen > data_.size()) {
            err_ = "中央目录文件名越界";
            return false;
        }
        e.name.assign(data_, p + 46, nameLen);
        // 兼容非标准 ZIP：部分工具（如 .NET ZipFile）会把 / 写成反斜杠
        for (char& ch : e.name) {
            if (ch == '\\') ch = '/';
        }
        entries_.push_back(e);

        p += 46 + nameLen + extraLen + commentLen;
    }

    return true;
}

const Reader::Entry* Reader::find(const std::string& name) const {
    for (const auto& e : entries_) {
        if (e.name == name) return &e;
    }
    // 兼容大小写差异（ZIP 规范文件名区分大小写，但实际文件常不一致）
    for (const auto& e : entries_) {
        if (e.name.size() == name.size()) {
            bool same = true;
            for (size_t i = 0; i < name.size(); i++) {
                unsigned char a = (unsigned char)e.name[i];
                unsigned char b = (unsigned char)name[i];
                if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
                if (a != b) { same = false; break; }
            }
            if (same) return &e;
        }
    }
    return nullptr;
}

bool Reader::has(const std::string& name) const {
    return find(name) != nullptr;
}

std::vector<std::string> Reader::names() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) out.push_back(e.name);
    return out;
}

bool Reader::read(const std::string& name, std::string& out) const {
    out.clear();
    const Entry* e = find(name);
    if (!e) {
        err_ = "ZIP 中没有该条目: " + name;
        return false;
    }

    size_t p = e->localOffset;
    if (p + 30 > data_.size() || rd32(data_, p) != 0x04034b50u) {
        err_ = "本地文件头损坏";
        return false;
    }
    unsigned int nameLen = rd16(data_, p + 26);
    unsigned int extraLen = rd16(data_, p + 28);
    size_t dataStart = p + 30 + nameLen + extraLen;
    if (dataStart + e->compSize > data_.size()) {
        err_ = "压缩数据越界";
        return false;
    }

    if (e->method == 0) {
        out.assign(data_, dataStart, e->compSize);
        return true;
    }
    if (e->method != 8) {
        err_ = "不支持的压缩方法: " + std::to_string(e->method);
        return false;
    }

    // deflate：从原始流起点开始（跳过 zlib 头，用 -15 的窗口）
    out.resize(e->uncompSize > 0 ? e->uncompSize : 1);
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) {
        err_ = "zlib 初始化失败";
        return false;
    }
    zs.next_in = (Bytef*)(data_.data() + dataStart);
    zs.avail_in = e->compSize;
    zs.next_out = (Bytef*)&out[0];
    zs.avail_out = (uInt)out.size();

    int rc = inflate(&zs, Z_FINISH);
    size_t produced = out.size() - zs.avail_out;
    inflateEnd(&zs);

    if (rc != Z_STREAM_END) {
        err_ = "解压失败";
        return false;
    }
    out.resize(produced);
    return true;
}

} // namespace zip
