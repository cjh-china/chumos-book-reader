#include "parser.h"
#include "fs_util.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

static std::vector<std::string> getArgs() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return out;
    for (int i = 0; i < argc; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(need > 0 ? (size_t)need - 1 : 0, '\0');
        if (need > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], need, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(wargv);
    return out;
}
#else
static std::vector<std::string> getArgs() {
    return std::vector<std::string>();
}
#endif

static void dump(const std::string& path, size_t maxLines) {
    Book book;
    bool ok = Parser::parse(path, book);
    printf("==== %s\n", path.c_str());
    printf("ok=%d title=[%s] author=[%s]\n", (int)ok, book.title.c_str(), book.author.c_str());
    printf("lines=%zu chapters=%zu\n", book.content.size(), book.chapters.size());
    for (size_t i = 0; i < book.chapters.size() && i < 10; i++) {
        printf("  chapter[%zu] @%d: %s\n", i, book.chapterStarts[i], book.chapters[i].c_str());
    }
    for (size_t i = 0; i < book.content.size() && i < maxLines; i++) {
        printf("  %3zu| %s\n", i, book.content[i].c_str());
    }
    printf("\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    std::vector<std::string> args = getArgs();
#else
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(argv[i]);
#endif
    if (args.size() < 2) {
        printf("usage: parser_test <file> [maxLines]\n");
        return 1;
    }
    size_t maxLines = args.size() >= 3 ? (size_t)atoi(args[2].c_str()) : 40;
    dump(args[1], maxLines);
    return 0;
}
