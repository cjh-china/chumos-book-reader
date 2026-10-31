#include "encoding.h"

#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#include <cerrno>
#endif

namespace encoding {

bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i++; continue; }

        int len = 0;
        unsigned int cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;

        if (i + len > n) return false;
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }

        // 拒绝过长编码与代理区
        if ((len == 2 && cp < 0x80) ||
            (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000)) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;

        i += len;
    }
    return true;
}

namespace {

#ifdef _WIN32

std::string cpToUtf8(const char* data, size_t size, UINT cp) {
    if (size == 0) return "";
    int need = MultiByteToWideChar(cp, 0, data, (int)size, nullptr, 0);
    if (need <= 0) return "";
    std::vector<wchar_t> wbuf((size_t)need);
    int got = MultiByteToWideChar(cp, 0, data, (int)size, wbuf.data(), need);
    if (got <= 0) return "";

    int u8need = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), got, nullptr, 0, nullptr, nullptr);
    if (u8need <= 0) return "";
    std::string out((size_t)u8need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), got, &out[0], u8need, nullptr, nullptr);
    return out;
}

std::string utf16leToUtf8(const char* data, size_t size) {
    // 跳过 BOM
    const unsigned char* p = (const unsigned char*)data;
    size_t off = 0;
    if (size >= 2 && p[0] == 0xFF && p[1] == 0xFE) off = 2;
    return cpToUtf8(data + off, size - off, 1200 /* UTF-16LE */);
}

std::string utf16beToUtf8(const char* data, size_t size) {
    const unsigned char* p = (const unsigned char*)data;
    size_t off = 0;
    if (size >= 2 && p[0] == 0xFE && p[1] == 0xFF) off = 2;
    return cpToUtf8(data + off, size - off, 1201 /* UTF-16BE */);
}

#else // POSIX

std::string iconvConvert(const char* from, const char* to, const char* data, size_t size) {
    if (size == 0) return "";
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return "";

    std::vector<char> outBuf(size * 4 + 64);
    char* inPtr = const_cast<char*>(data);
    size_t inLeft = size;
    char* outPtr = outBuf.data();
    size_t outLeft = outBuf.size();

    while (inLeft > 0) {
        size_t r = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
        if (r == (size_t)-1) {
            if (errno == E2BIG) {
                size_t used = (size_t)(outPtr - outBuf.data());
                outBuf.resize(outBuf.size() * 2);
                outPtr = outBuf.data() + used;
                outLeft = outBuf.size() - used;
                continue;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                // 非法字节：跳过一字节继续（尽量不丢整篇内容）
                if (inLeft > 0) { inPtr++; inLeft--; *outPtr++ = '?'; outLeft--; continue; }
            }
            iconv_close(cd);
            return "";
        }
    }
    iconv_close(cd);
    return std::string(outBuf.data(), (size_t)(outPtr - outBuf.data()));
}

std::string utf16leToUtf8(const char* data, size_t size) {
    const unsigned char* p = (const unsigned char*)data;
    size_t off = 0;
    if (size >= 2 && p[0] == 0xFF && p[1] == 0xFE) off = 2;
    return iconvConvert("UTF-16LE", "UTF-8", data + off, size - off);
}

std::string utf16beToUtf8(const char* data, size_t size) {
    const unsigned char* p = (const unsigned char*)data;
    size_t off = 0;
    if (size >= 2 && p[0] == 0xFE && p[1] == 0xFF) off = 2;
    return iconvConvert("UTF-16BE", "UTF-8", data + off, size - off);
}

std::string gb18030ToUtf8(const char* data, size_t size) {
    return iconvConvert("GB18030", "UTF-8", data, size);
}

#endif

std::string gbToUtf8(const char* data, size_t size) {
#ifdef _WIN32
    std::string r = cpToUtf8(data, size, 54936 /* GB18030 */);
    if (!r.empty()) return r;
    return cpToUtf8(data, size, 936 /* GBK */);
#else
    std::string r = gb18030ToUtf8(data, size);
    if (!r.empty()) return r;
    return iconvConvert("GBK", "UTF-8", data, size);
#endif
}

} // namespace

std::string toUtf8(const std::string& raw) {
    if (raw.empty()) return raw;

    const unsigned char* p = (const unsigned char*)raw.data();
    size_t n = raw.size();

    // 1. BOM 判定
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        return raw.substr(3);  // UTF-8 BOM
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        std::string r = utf16leToUtf8(raw.data(), n);
        if (!r.empty()) return r;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        std::string r = utf16beToUtf8(raw.data(), n);
        if (!r.empty()) return r;
    }

    // 2. 无 BOM：先验 UTF-8
    if (isValidUtf8(raw)) return raw;

    // 3. 退回 GB18030
    std::string r = gbToUtf8(raw.data(), n);
    if (!r.empty()) return r;

    // 4. 兜底：原样返回
    return raw;
}

} // namespace encoding
