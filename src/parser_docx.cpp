#include "parser.h"
#include "zip_reader.h"
#include "markup_util.h"
#include "fs_util.h"

#include <vector>

namespace {

std::string toLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

// DOCX 的 document.xml → 段落文本行
// 规则：
//   <w:p>...</w:p>      一个段落（生成一行，空段落保留为空行）
//   <w:t>...</w:t>      文本片段
//   <w:tab/>           制表符 → 两个空格
//   <w:br/> <w:cr/>    换行 → 段落内换行
//   <w:delText>        忽略（修订删除内容）
//   <w:instrText>      忽略（域代码）
std::vector<std::string> docxParagraphs(const std::string& xml) {
    std::vector<std::string> lines;

    size_t p = 0;
    std::string cur;
    bool inText = false;
    bool skip = false;

    auto pushLine = [&]() {
        // 去掉行尾空白
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
        // Word 把引号/尖括号等写成 XML 实体，需要还原
        lines.push_back(markup::decodeEntities(cur));
        cur.clear();
    };

    while (p < xml.size()) {
        if (xml[p] != '<') {
            if (inText && !skip) cur += xml[p];
            p++;
            continue;
        }

        size_t end = xml.find('>', p);
        if (end == std::string::npos) break;

        std::string inner = xml.substr(p + 1, end - p - 1);
        bool closing = !inner.empty() && inner[0] == '/';
        std::string body = closing ? inner.substr(1) : inner;
        bool selfClose = !body.empty() && body.back() == '/';
        if (selfClose) body.pop_back();

        // 取标签名（可能带命名空间前缀）
        size_t ne = 0;
        while (ne < body.size() && body[ne] != ' ' && body[ne] != '\t' &&
               body[ne] != '\n' && body[ne] != '\r') {
            ne++;
        }
        std::string name = toLower(body.substr(0, ne));

        if (name == "w:p") {
            if (closing) {
                pushLine();
            } else if (selfClose) {
                pushLine();
            }
            // 段落开始不清空 cur（正常情况下已为空）
        } else if (name == "w:t") {
            if (closing) inText = false;
            else if (selfClose) { /* 空文本节点 */ }
            else inText = true;
        } else if (name == "w:deltext" || name == "w:instrtext") {
            if (closing) skip = false;
            else if (!selfClose) skip = true;
        } else if (name == "w:tab") {
            if (!skip) cur += "  ";
        } else if (name == "w:br" || name == "w:cr") {
            if (!skip) {
                pushLine();
            }
        }

        p = end + 1;
    }

    if (!cur.empty()) pushLine();

    return lines;
}

} // namespace

bool Parser::parseDOCX(const std::string& filename, Book& book) {
    zip::Reader zr;
    if (!zr.open(filename)) {
        book.title = fsutil::stem(filename);
        book.content.push_back("无法打开 DOCX：" + zr.lastError());
        return true;
    }

    std::string doc;
    if (!zr.read("word/document.xml", doc)) {
        book.title = fsutil::stem(filename);
        book.content.push_back("DOCX 中找不到 word/document.xml");
        return true;
    }

    // 元信息（docProps/core.xml 可选）
    book.title = fsutil::stem(filename);
    book.author.clear();
    std::string core;
    if (zr.read("docProps/core.xml", core)) {
        std::string t = markup::firstTagText(core, "dc:title");
        if (!t.empty()) book.title = t;
        std::string a = markup::firstTagText(core, "dc:creator");
        if (!a.empty()) book.author = a;
    }

    std::vector<std::string> lines = docxParagraphs(doc);

    // 去掉首尾连续空行
    size_t begin = 0;
    while (begin < lines.size() && lines[begin].empty()) begin++;
    size_t endIdx = lines.size();
    while (endIdx > begin && lines[endIdx - 1].empty()) endIdx--;

    if (begin >= endIdx) {
        book.content.push_back("（DOCX 中没有可提取的文本）");
        return true;
    }

    // 连续空行折叠为一个
    bool prevEmpty = false;
    for (size_t i = begin; i < endIdx; i++) {
        const std::string& line = lines[i];
        if (line.empty()) {
            if (prevEmpty) continue;
            prevEmpty = true;
        } else {
            prevEmpty = false;
        }
        book.content.push_back(line);
    }

    // 章节识别：与 TXT 共用同一套规则
    for (size_t i = 0; i < book.content.size(); i++) {
        if (Parser::isChapterTitle(book.content[i])) {
            book.chapters.push_back(book.content[i]);
            book.chapterStarts.push_back((int)i);
        }
    }

    return true;
}
