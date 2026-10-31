#ifndef ENCODING_H
#define ENCODING_H

#include <string>

namespace encoding {

// 检测文本编码并统一转换为 UTF-8。
// 支持：UTF-8(BOM/无BOM)、UTF-16LE/BE(BOM)、GB18030/GBK(无BOM 且非 UTF-8)
std::string toUtf8(const std::string& raw);

// 严格校验是否为合法 UTF-8
bool isValidUtf8(const std::string& s);

} // namespace encoding

#endif
