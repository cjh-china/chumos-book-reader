#include "reader.h"
#include "fs_util.h"
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef HAS_MUSIC
#include "music_ui.h"
#include "player.h"
#include "playlist_store.h"
#endif

#ifdef HAS_CURL
#include "fanqie_downloader.h"
#endif

#ifdef HAS_CURL
static int runFanqie() {
    FanqieDownloader dl(8);
    while (true) {
        std::cout << "\n请输入书名 (输入 q 退出): ";
        std::string kw;
        if (!std::getline(std::cin, kw)) break;
        if (kw == "q" || kw == "Q") break;
        if (kw.empty()) continue;

        std::cout << "搜索「" << kw << "」..." << std::endl;
        auto books = dl.searchBooks(kw);
        if (books.empty()) {
            std::cout << "未找到相关书籍" << std::endl;
            continue;
        }

        std::cout << "\n找到 " << books.size() << " 本:\n";
        for (size_t i = 0; i < books.size(); i++) {
            std::cout << "  " << (i + 1) << ". 《" << books[i].book_name << "》 - "
                      << books[i].author;
            if (!books[i].score.empty()) std::cout << " [评分" << books[i].score << "]";
            std::cout << "\n";
            if (!books[i].abstract.empty())
                std::cout << "      " << books[i].abstract << "...\n";
        }

        std::cout << "\n请选择序号 (回车取消): ";
        std::string sel;
        if (!std::getline(std::cin, sel)) break;
        if (sel.empty()) continue;
        int choice = 0;
        try { choice = std::stoi(sel); } catch (...) { choice = 0; }
        if (choice < 1 || choice > (int)books.size()) {
            std::cout << "输入无效" << std::endl;
            continue;
        }

        std::cout << "每本下载章节数 (回车=全部): ";
        std::string lim;
        if (!std::getline(std::cin, lim)) break;
        int mx = -1;
        if (!lim.empty()) { try { mx = std::stoi(lim); } catch (...) { mx = -1; } }

        const auto& b = books[choice - 1];
        try {
            dl.downloadBook(b.book_id, b.book_name, "", mx);
        } catch (const std::exception& e) {
            std::cout << "下载出错: " << e.what() << std::endl;
        }
    }
    return 0;
}
#endif

static void printUsage(const char* prog) {
    std::cerr << "用法:" << std::endl;
    std::cerr << "  " << prog << "                    打开书库" << std::endl;
    std::cerr << "  " << prog << " <文件路径或URL>    直接打开" << std::endl;
    std::cerr << "  " << prog << " --fanqie           番茄小说下载" << std::endl;
    std::cerr << std::endl;
    std::cerr << "支持格式: TXT(自动识别 UTF-8/UTF-16/GBK), EPUB, DOCX, PDF" << std::endl;
    std::cerr << "也支持 http:// 或 https:// 链接（需要 libcurl）" << std::endl;
    std::cerr << std::endl;
    std::cerr << "配置文件: 当前目录下的 ebook_reader.config.json" << std::endl;
}

#ifdef _WIN32
// Windows 下 main 的 argv 是 ANSI 代码页编码，统一转成 UTF-8。
// 通过 GetCommandLineW 取原始宽字符命令行，避免中文路径被 ANSI 截断。
static std::vector<std::string> getUtf8Args() {
    std::vector<std::string> out;

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return out;

    for (int i = 0; i < argc; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        if (need <= 0) { out.push_back(std::string()); continue; }
        std::string s((size_t)need, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], need, nullptr, nullptr);
        if (!s.empty() && s.back() == '\0') s.pop_back();
        out.push_back(s);
    }

    LocalFree(wargv);
    return out;
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    std::vector<std::string> args;
#ifdef _WIN32
    args = getUtf8Args();
    if (args.empty()) {
        for (int i = 0; i < argc; i++) args.push_back(argv[i]);
    }
#else
    for (int i = 0; i < argc; i++) args.push_back(argv[i]);
#endif

    std::string arg;
    if (args.size() >= 2) {
        arg = args[1];
        if (arg == "-h" || arg == "--help") {
            printUsage(args[0].c_str());
            return 0;
        }
        if (arg == "--fanqie" || arg == "fanqie") {
#ifdef HAS_CURL
            return runFanqie();
#else
            std::cerr << "番茄下载功能需要 libcurl，请在构建时启用。" << std::endl;
            return 1;
#endif
        }
    }

    TUI tui;
    Reader reader(&tui);

    if (!reader.initConfig()) {
        std::cerr << "警告: 配置文件读写失败，本次运行不会保存记录" << std::endl;
    }

    if (!tui.init()) {
        std::cerr << "无法初始化终端界面" << std::endl;
        return 1;
    }

    std::string target = arg;

    // 命令行直接指定了文件：先读它，退出阅读后仍回到书库
    if (!target.empty()) {
        reader.setAutoFallback(false);
        std::string reason;
        if (!reader.loadBookOrRecover(target, reason)) {
            tui.shutdown();
            std::cerr << "错误: 无法加载 " << target << "（" << reason << "）" << std::endl;
            return 1;
        }
        target.clear();
        reader.run(false);
        reader.clearMemoryFlag();
    }

#ifdef HAS_MUSIC
    // 音乐模块按需初始化（避免不用时占用音频设备）
    Player musicPlayer;
    MusicStore musicStore;
    bool musicInited = false;
#endif

    // 书库主循环
    while (true) {
        std::string pick = reader.showLibrary();

        if (pick.empty()) {
            if (reader.hasBookInMemory()) {
                // 番茄搜索已直接载入内存
                reader.run(false);
                reader.clearMemoryFlag();
                continue;
            }

#ifdef HAS_MUSIC
            if (reader.wantsMusic()) {
                reader.clearMusicRequest();

                if (!musicInited) {
                    musicStore.load(".");
                    if (!musicPlayer.init()) {
                        int h = tui.getHeight();
                        tui.clear();
                        tui.drawText(2, 2, "音频设备初始化失败：" + musicPlayer.lastError());
                        tui.drawText(h - 1, 0, "  按任意键返回书库");
                        tui.refresh();
                        tui.getKey();
                        continue;
                    }
                    musicInited = true;
                }

                MusicUI musicUI(&tui, &musicPlayer, &musicStore);
                musicUI.run();
                continue;
            }
#endif

            break;  // 用户退出书库
        }

        // 文件丢失时尝试从番茄补下载同名书
        reader.setAutoFallback(true);

        int h = tui.getHeight();
        int w = tui.getWidth();

        if (!fsutil::exists(pick)) {
            tui.clear();
            tui.drawText(2, 2, "文件不存在，正在番茄搜索同名书籍...");
            tui.drawLine(3, 2, pick, w - 4);
            tui.refresh();
        }

        std::string reason;
        if (!reader.loadBookOrRecover(pick, reason)) {
            tui.clear();
            tui.drawText(2, 2, "无法加载：");
            tui.drawLine(3, 2, pick, w - 4);
            tui.drawText(5, 2, reason);
            tui.drawText(h - 1, 0, "  按任意键返回书库");
            tui.refresh();
            tui.getKey();
            continue;
        }

        reader.run(false);
    }

#ifdef HAS_MUSIC
    if (musicInited) musicPlayer.shutdown();
#endif
    tui.shutdown();
    return 0;
}
