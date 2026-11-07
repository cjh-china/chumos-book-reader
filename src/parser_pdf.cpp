#include "parser.h"
#include "fs_util.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <regex>
#include <cctype>

namespace {

// 读取文件所有内容
std::string readAll(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// 简易 PDF 解码：提取文本流
// 支持：简单字符串 (text) Tj、数组 [(str) num] TJ
// 不支持：复杂字体编码、CMap、CID 字体、FlateDecode 压缩流
std::string decodePdfStream(const std::string& streamData) {
    std::string out;
    std::smatch m;
    std::string data = streamData;

    // 1. 简单字符串操作符：(text) Tj
    std::regex simpleStr("\\(([^)]*)\\)\\s*Tj");
    std::string::const_iterator it = data.cbegin();
    while (std::regex_search(it, data.cend(), m, simpleStr)) {
        std::string s = m[1].str();
        // 处理 PDF 字符串转义（括号、反斜杠、八进制）
        std::string decoded;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char nxt = s[i + 1];
                if (nxt == '(' || nxt == ')' || nxt == '\\') decoded += nxt;
                else if (nxt == 'n') decoded += '\n';
                else if (nxt == 'r') decoded += '\r';
                else if (nxt == 't') decoded += '\t';
                else if (nxt == 'b') decoded += '\b';
                else if (nxt == 'f') decoded += '\f';
                else if (i + 3 < s.size() && std::isdigit(s[i+1]) && std::isdigit(s[i+2]) && std::isdigit(s[i+3])) {
                    // 八进制转义 \ddd
                    int v = (s[i+1]-'0')*64 + (s[i+2]-'0')*8 + (s[i+3]-'0');
                    decoded += (char)v;
                    i += 3;
                } else {
                    decoded += nxt;
                }
                ++i;
            } else {
                decoded += s[i];
            }
        }
        out += decoded + " ";
        it = m.suffix().first;
    }

    // 2. 数组操作符：[(str) num (str) num] TJ
    std::regex arrayStr("\\[([^\\]]*)\\]\\s*TJ");
    it = data.cbegin();
    std::smatch arrMatch;
    while (std::regex_search(it, data.cend(), arrMatch, arrayStr)) {
        std::string arr = arrMatch[1].str();
        // 提取数组中的字符串
        std::regex innerStr("\\(([^)]*)\\)");
        std::string::const_iterator it2 = arr.cbegin();
        // 内层必须用独立的 match 对象：复用外层 m 会让外层的 suffix()
        // 指向已销毁的局部字符串 arr，且迭代器类型也不一致
        std::smatch innerMatch;
        while (std::regex_search(it2, arr.cend(), innerMatch, innerStr)) {
            std::string s = innerMatch[1].str();
            std::string decoded;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char nxt = s[i + 1];
                    if (nxt == '(' || nxt == ')' || nxt == '\\') decoded += nxt;
                    else if (nxt == 'n') decoded += '\n';
                    else if (nxt == 'r') decoded += '\r';
                    else if (nxt == 't') decoded += '\t';
                    else if (nxt == 'b') decoded += '\b';
                    else if (nxt == 'f') decoded += '\f';
                    else if (i + 3 < s.size() && std::isdigit(s[i+1]) && std::isdigit(s[i+2]) && std::isdigit(s[i+3])) {
                        int v = (s[i+1]-'0')*64 + (s[i+2]-'0')*8 + (s[i+3]-'0');
                        decoded += (char)v;
                        i += 3;
                    } else {
                        decoded += nxt;
                    }
                    ++i;
                } else {
                    decoded += s[i];
                }
            }
            out += decoded;
            it2 = innerMatch.suffix().first;
        }
        out += " ";
        it = arrMatch.suffix().first;
    }

    return out;
}

// 提取所有 stream...endstream 块
std::vector<std::string> extractStreams(const std::string& pdf) {
    std::vector<std::string> streams;
    std::regex streamRe("stream\\s*\\r?\\n([\\s\\S]*?)\\r?\\nendstream");
    std::smatch m;
    std::string::const_iterator it = pdf.begin();
    while (std::regex_search(it, pdf.end(), m, streamRe)) {
        streams.push_back(m[1].str());
        it = m.suffix().first;
    }
    return streams;
}

// 尝试提取 /Title、/Author 等元信息（来自 Info 字典）
std::string extractInfo(const std::string& pdf, const std::string& key) {
    // 找 /Info 字典
    std::regex infoRe("/Info\\s*<<([^>]*?)>>");
    std::smatch m;
    if (std::regex_search(pdf, m, infoRe)) {
        std::string info = m[1].str();
        std::regex keyRe("/" + key + "\\s*\\(([^)]*)\\)");
        if (std::regex_search(info, m, keyRe)) {
            return m[1].str();
        }
    }
    return "";
}

// 清理提取的文本
std::string cleanText(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ) {
        unsigned char c = raw[i];
        if ((c & 0x80) == 0) {
            if (c >= 32 && c <= 126) out += c;
            else if (c == '\n' || c == '\r' || c == '\t') out += ' ';
            i++;
        } else {
            // 粗略保留 UTF-8 多字节
            int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (i + len <= raw.size()) {
                out.append(raw.substr(i, len));
                i += len;
            } else break;
        }
    }
    // 合并多余空白
    std::regex ws("\\s+");
    out = std::regex_replace(out, ws, " ");
    return out;
}

// 文本按行切分，尝试按句子边界换行
std::vector<std::string> splitIntoLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        cur += c;
        // 句号、问号、感叹号、换行符作为句子结束
        if (c == '。' || c == '！' || c == '？' || c == '.' || c == '?' || c == '!') {
            if (i + 1 < text.size() && (text[i+1] == '"' || text[i+1] == '\'' || text[i+1] == '」' || text[i+1] == '』')) {
                cur += text[++i];
            }
            // trim
            size_t a = 0, b = cur.size();
            while (a < b && std::isspace((unsigned char)cur[a])) a++;
            while (b > a && std::isspace((unsigned char)cur[b-1])) b--;
            if (b > a) lines.push_back(cur.substr(a, b-a));
            cur.clear();
        }
    }
    if (!cur.empty()) {
        size_t a = 0, b = cur.size();
        while (a < b && std::isspace((unsigned char)cur[a])) a++;
        while (b > a && std::isspace((unsigned char)cur[b-1])) b--;
        if (b > a) lines.push_back(cur.substr(a, b-a));
    }
    return lines;
}

} // namespace

bool Parser::parsePDF(const std::string& filename, Book& book) {
    std::string data = readAll(filename);
    if (data.empty()) return false;

    // 检查 PDF 头
    if (data.size() < 5 || data.substr(0, 5) != "%PDF-") {
        book.content.push_back("Not a valid PDF file");
        return true;
    }

    // 从文件名取标题
    size_t slash = filename.find_last_of("/\\");
    std::string fname = (slash == std::string::npos) ? filename : filename.substr(slash + 1);
    size_t dot = fname.find_last_of('.');
    if (dot != std::string::npos) fname = fname.substr(0, dot);
    book.title = fname;

    // 尝试从 Info 字典读取元信息
    std::string title = extractInfo(data, "Title");
    std::string author = extractInfo(data, "Author");
    if (!title.empty()) book.title = title;
    if (!author.empty()) book.author = author;

    // 提取所有流并解码文本
    std::string allText;
    auto streams = extractStreams(data);
    for (const auto& stream : streams) {
        allText += decodePdfStream(stream);
    }

    if (!allText.empty()) {
        // 清理
        allText = cleanText(allText);
        // 按句子/行切分
        auto lines = splitIntoLines(allText);
        for (const auto& line : lines) {
            if (!line.empty()) book.content.push_back(line);
        }
    } else {
        book.content.push_back("[PDF 文本提取受限：该文件可能使用了复杂编码、字体子集、CMap 或压缩流]");
        book.content.push_back("[建议使用 Poppler/MuPDF 等专业库，或先转为 TXT 再阅读]");
    }

    return true;
}