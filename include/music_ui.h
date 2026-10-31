#ifndef MUSIC_UI_H
#define MUSIC_UI_H

#include "tui.h"
#include "music.h"
#include "player.h"
#include "playlist_store.h"
#include "online_music.h"

#include <string>
#include <vector>

// 音乐播放器 TUI 界面
class MusicUI {
public:
    MusicUI(TUI* tui, Player* player, MusicStore* store);
    ~MusicUI();

    // 主入口：返回时表示退出音乐模块
    void run();

private:
    // 界面
    void drawPlayer();
    void drawLibrary();
    void drawPlaylists();
    void drawOnline();
    void drawTagEditor();
    void drawHelp();

    // 交互
    void handlePlayerKey(int key);
    void handleLibraryKey(int key);
    void handlePlaylistKey(int key);
    void handleOnlineKey(int key);
    void handleTagEditorKey(int key);

    // 操作
    void rescanLibrary();
    void showAddDirDialog();
    void playSelected();
    void showPlaylistPicker();
    void showCreatePlaylistDialog();
    void showMessage(const std::string& msg, int ms = 900);
    void doOnlineSearch();
    void playOnlineSelected();
    void downloadOnlineSelected();
    void downloadOnlineMarked();
    void showTagEditor();
    void showSleepTimerDialog();

    std::string fmtTime(double sec) const;
    std::string truncateUtf8(const std::string& s, int maxWidth) const;
    int displayWidth(const std::string& s) const;

    TUI* tui;
    Player* player;
    MusicStore* store;

    // 曲库
    std::vector<Song> library;
    std::vector<int> filtered;        // 过滤后的索引
    std::string filter;
    int libCursor = 0;
    int libOffset = 0;

    // 界面状态
    enum class Page { Player, Library, Playlists, Online, TagEditor };
    Page page = Page::Library;

    // 在线搜索
    OnlineMusicService online;
    std::vector<OnlineSong> onlineResults;
    int onlineCursor = 0;
    int onlineOffset = 0;
    std::string onlineQuery;
    std::string onlineSource;   // 空表示全部
    std::vector<bool> onlineMarked;   // 批量下载标记
    bool onlineDownloading = false;
    std::string onlineDlMsg;

    // 标签编辑
    Song tagSong;
    int tagField = 0;            // 0=标题 1=艺术家 2=专辑
    std::string tagMsg;

    // 歌单页
    int plCursor = 0;
    bool inPlaylistDetail = false;
    int detailCursor = 0;

    // 歌词
    LyricDoc lyric;
    std::string lyricPath;
    bool showLyric = false;

    // 循环
    bool running = true;
    std::string statusMsg;
    long long statusUntilMs = 0;

    void loadLyricForCurrent();
    void refreshFilter();
};

#endif
