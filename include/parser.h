#ifndef PARSER_H
#define PARSER_H

#include "reader.h"
#include <string>

class Parser {
public:
    static bool parse(const std::string& filename, Book& book);

    // 判断一行是否像章节标题。
    // 规则：去掉前导空白后以「第」开头 + 含 章/节/回/卷/篇，或匹配 Chapter N；
    // 并且长度较短（排除正文里顺带提到"第一章"的句子）。
    static bool isChapterTitle(const std::string& line);
    
private:
    static std::string getExtension(const std::string& filename);
    static bool parseTXT(const std::string& filename, Book& book);
    static bool parseEPUB(const std::string& filename, Book& book);
    static bool parseDOCX(const std::string& filename, Book& book);
    static bool parsePDF(const std::string& filename, Book& book);
};

#endif
