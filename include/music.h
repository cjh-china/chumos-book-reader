#ifndef MUSIC_H
#define MUSIC_H

#include <string>
#include <vector>

// ==================== 数据模型 ====================

struct Song {
    std::string path;          // 文件绝对路径（本地）或 URL（远程）
    std::string title;
    std::string artist;
    std::string album;
    std::string lyricPath;     // 关联的 .lrc（可空）
    std::string coverPath;     // 关联的封面图（可空）
    double durationSec = 0;    // 时长，0 表示未知
    long long fileSize = 0;
    long long mtimeMs = 0;
    bool remote = false;
};

struct LyricLine {
    double timeSec = 0;
    std::string text;
};

struct LyricDoc {
    std::vector<LyricLine> lines;
    bool valid = false;

    // 返回当前时间应高亮的行索引；无匹配返回 -1
    int indexAt(double t) const;
};

// ==================== LRC 解析 ====================

namespace lrc {

// 解析 LRC 文本。支持：
//   [mm:ss.xx] / [mm:ss.xxx] / [mm:ss]
//   一行多时间标签 [00:12.50][01:20.00]歌词
//   元信息标签 [ti:][ar:][al:][offset:]
LyricDoc parse(const std::string& text);

// 从文件读取并解析（自动识别 UTF-8 / GBK / UTF-16）
LyricDoc parseFile(const std::string& path);

}

// ==================== 曲库扫描 ====================

namespace musiclib {

// 支持的音频扩展名
bool isAudioFile(const std::string& path);

// 递归扫描目录，返回找到的歌曲（读取基础标签）
std::vector<Song> scanDir(const std::string& dir, int maxDepth = 8);

// 从文件读取标签（ID3v1/v2、FLAC Vorbis Comment、文件名兜底）
Song readMetadata(const std::string& path);

// 在音频文件同目录查找同名 .lrc / .jpg
void attachSidecarFiles(Song& song);

// 写入标签。成功返回 true，失败时 error 给出原因。
// MP3: 改写 ID3v2.3（保留原帧，仅更新 TIT2/TPE1/TALB）
// FLAC: 改写 Vorbis Comment
bool writeTags(const std::string& path,
               const std::string& title,
               const std::string& artist,
               const std::string& album,
               std::string& error);

}

#endif
