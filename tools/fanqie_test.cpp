#include "fanqie_downloader.h"
#include <iostream>
#include <chrono>

int main() {
    std::cout << "=== 测试自动拉起服务 ===" << std::endl;
    auto t0 = std::chrono::steady_clock::now();

    FanqieDownloader dl(4);
    bool ok = dl.ensureService();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "ensureService: " << (ok ? "成功" : "失败")
              << "，耗时 " << ms << " ms" << std::endl;
    if (!ok) return 1;

    std::cout << "=== 测试搜索 ===" << std::endl;
    auto books = dl.searchBooks("斗罗");
    std::cout << "结果数: " << books.size() << std::endl;
    for (size_t i = 0; i < books.size() && i < 3; i++) {
        std::cout << "  " << (i + 1) << ". 《" << books[i].book_name
                  << "》 - " << books[i].author
                  << " [评分" << books[i].score << "]" << std::endl;
    }

    if (books.empty()) return 2;

    std::cout << "=== 测试下载前 2 章 ===" << std::endl;
    std::string text = dl.buildBookText(books[0].book_id, 2);
    std::cout << "正文长度: " << text.size() << " 字节" << std::endl;
    if (!text.empty()) {
        std::cout << "--- 前 300 字节 ---" << std::endl;
        std::cout << text.substr(0, 300) << std::endl;
    }
    return text.empty() ? 3 : 0;
}
