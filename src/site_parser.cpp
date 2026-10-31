#include "site_parser.h"
#include "http_client.h"
#include <iostream>
#include <algorithm>
#include <regex>

bool SiteParser::isTomatoNovel(const std::string& url) {
    return url.find("fanqienovel.com") != std::string::npos || 
           url.find("tomato") != std::string::npos;
}

bool SiteParser::parseUrl(const std::string& url, Book& book) {
#ifdef HAS_CURL
    if (isTomatoNovel(url)) {
        return parseTomatoNovel(url, book);
    }
#endif
    return false;
}

std::string SiteParser::cleanHtml(const std::string& html) {
    std::string result = html;
    // 移除 script 和 style 标签
    std::regex scriptRegex("<script[^>]*>[\\s\\S]*?</script>", std::regex_constants::icase);
    result = std::regex_replace(result, scriptRegex, "");
    std::regex styleRegex("<style[^>]*>[\\s\\S]*?</style>", std::regex_constants::icase);
    result = std::regex_replace(result, styleRegex, "");
    // 移除 HTML 标签
    std::regex tagRegex("<[^>]+>");
    result = std::regex_replace(result, tagRegex, "");
    // 处理 HTML 实体
    result = std::regex_replace(result, std::regex("&nbsp;"), " ");
    result = std::regex_replace(result, std::regex("&lt;"), "<");
    result = std::regex_replace(result, std::regex("&gt;"), ">");
    result = std::regex_replace(result, std::regex("&amp;"), "&");
    result = std::regex_replace(result, std::regex("&#\\d+;"), "");
    return result;
}

bool SiteParser::parseTomatoNovel(const std::string& url, Book& book) {
#ifdef HAS_CURL
    HttpClient client;
    std::string html = client.get(url);
    if (html.empty()) {
        return false;
    }
    
    book.title = "番茄小说";
    book.author = "";
    book.content.clear();
    
    // 简单提取：这里仅作框架示例
    // 实际使用需要根据番茄小说的具体DOM结构调整
    book.content.push_back("【在线阅读功能框架】");
    book.content.push_back("");
    book.content.push_back("由于番茄小说有反爬机制和法律限制，");
    book.content.push_back("此框架仅供学习和解析公开内容使用。");
    book.content.push_back("");
    book.content.push_back("使用前请遵守相关网站的用户协议。");
    book.content.push_back("");
    book.content.push_back("建议优先使用本地下载的txt文件阅读。");
    
    return true;
#else
    return false;
#endif
}

std::vector<SiteParser::Chapter> SiteParser::getChapterList(const std::string& baseUrl) {
    (void)baseUrl;
    std::vector<Chapter> chapters;
    return chapters;
}

std::string SiteParser::getChapterContent(const std::string& chapterUrl) {
    (void)chapterUrl;
    return "";
}
