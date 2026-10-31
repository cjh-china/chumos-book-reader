#ifndef SITE_PARSER_H
#define SITE_PARSER_H

#include <string>
#include <vector>
#include "reader.h"

class SiteParser {
public:
    struct Chapter {
        std::string title;
        std::string url;
    };
    
    static bool parseUrl(const std::string& url, Book& book);
    static std::vector<Chapter> getChapterList(const std::string& baseUrl);
    static std::string getChapterContent(const std::string& chapterUrl);
    
private:
    static bool isTomatoNovel(const std::string& url);
    static bool parseTomatoNovel(const std::string& url, Book& book);
    static std::string cleanHtml(const std::string& html);
    static std::string extractText(const std::string& html, const std::string& startTag, const std::string& endTag);
};

#endif
