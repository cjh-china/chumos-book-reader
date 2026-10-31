#ifndef MARKUP_UTIL_H
#define MARKUP_UTIL_H

#include <string>
#include <vector>

// EPUB(XHTML) / DOCX(XML) 解析用的小工具，全部按字节扫描，不做通用 DOM。
namespace markup {

// 解码常见实体：&amp; &lt; &gt; &quot; &apos; &#NNN; &#xHH;
std::string decodeEntities(const std::string& s);

// 把 HTML/XHTML 片段转成纯文本：
// - <p> <div> <br> <h1..6> <li> <tr> 作为段落/换行边界
// - 其余标签直接丢弃
// - 行内连续空白折叠为单个空格，去掉首尾空白
// - <script> <style> 内容整体丢弃
std::string htmlToText(const std::string& html);

// 取第一个 <tag ...>text</tag> 的 text（已去标签并解码实体）
std::string firstTagText(const std::string& xml, const std::string& tag);

// 取所有 <tag ...>...</tag> 的内部内容（原始，不去标签）
std::vector<std::string> allTagText(const std::string& xml, const std::string& tag);

} // namespace markup

#endif
