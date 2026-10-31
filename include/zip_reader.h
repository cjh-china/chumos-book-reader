#ifndef ZIP_READER_H
#define ZIP_READER_H

#include <string>
#include <vector>

// 最小 ZIP 读取器（只读，支持 stored / deflate）。
// 用于解析 EPUB、DOCX 这类基于 ZIP 的容器格式，依赖 zlib。
namespace zip {

class Reader {
public:
    Reader() = default;
    ~Reader() = default;

    // 打开 ZIP 文件：一次性读入内存并解析中央目录
    bool open(const std::string& path);

    bool has(const std::string& name) const;

    // 解压指定条目到 out；成功返回 true
    bool read(const std::string& name, std::string& out) const;

    // 全部条目名
    std::vector<std::string> names() const;

    const std::string& lastError() const { return err_; }

private:
    struct Entry {
        std::string name;
        unsigned short method = 0;
        unsigned int compSize = 0;
        unsigned int uncompSize = 0;
        unsigned int localOffset = 0;
        unsigned int crc = 0;
    };

    const Entry* find(const std::string& name) const;

    std::string data_;
    std::vector<Entry> entries_;
    mutable std::string err_;   // read() 为 const 方法，失败时仍要记录错误
};

} // namespace zip

#endif
