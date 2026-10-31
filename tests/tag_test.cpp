#include "music.h"
#include "fs_util.h"
#include <cstdio>
#include <string>
#include <vector>

static bool hasFrame(const std::string& path, const char* id) {
    std::string f;
    if (!fsutil::readAll(path, f)) return false;
    return f.find(std::string(id, 4)) != std::string::npos;
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    std::string path = argv[1];
    std::string err;

    // 1) 先写普通标签
    bool ok1 = musiclib::writeTags(path, "测试标题", "测试艺术家", "测试专辑", err);
    printf("[1] writeTags ok=%d err=%s\n", (int)ok1, err.c_str());
    printf("    TIT2=%d TPE1=%d TALB=%d\n",
           (int)hasFrame(path, "TIT2"), (int)hasFrame(path, "TPE1"), (int)hasFrame(path, "TALB"));
    Song s1 = musiclib::readMetadata(path);
    printf("    read back: [%s] [%s] [%s]\n",
           s1.title.c_str(), s1.artist.c_str(), s1.album.c_str());

    // 2) 再合成歌词与封面
    std::string lrc = "[00:00.00]第一行\n[00:03.00]第二行\n";
    std::string png = "\x89PNG\r\n\x1a\n";
    png.append(64, 'X');
    bool ok2 = musiclib::embedMetadata(path, lrc, png, "image/png", err);
    printf("[2] embedMetadata ok=%d err=%s\n", (int)ok2, err.c_str());

    // 3) 四个帧必须同时存在（原有 TIT2/TPE1/TALB 不能被冲掉）
    printf("    TIT2=%d TPE1=%d TALB=%d USLT=%d APIC=%d\n",
           (int)hasFrame(path, "TIT2"), (int)hasFrame(path, "TPE1"),
           (int)hasFrame(path, "TALB"), (int)hasFrame(path, "USLT"),
           (int)hasFrame(path, "APIC"));
    Song s2 = musiclib::readMetadata(path);
    printf("    read back: [%s] [%s] [%s]\n",
           s2.title.c_str(), s2.artist.c_str(), s2.album.c_str());

    // 4) 再合成一次（幂等：不应重复堆叠帧）
    bool ok3 = musiclib::embedMetadata(path, lrc, png, "image/png", err);
    std::string f;
    fsutil::readAll(path, f);
    int usltCount = 0;
    size_t p = 0;
    while ((p = f.find("USLT", p)) != std::string::npos) { usltCount++; p += 4; }
    printf("[3] re-embed ok=%d  USLT count=%d (应为 1)\n", (int)ok3, usltCount);
    printf("    final size=%lld\n", fsutil::fileSize(path));
    return 0;
}
