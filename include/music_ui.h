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
    void drawLogin();
    void drawNeteasePlaylists();
    void drawToplists();

    // 交互
    void handlePlayerKey(int key);
    void handleLibraryKey(int key);
    void handlePlaylistKey(int key);
    void handleOnlineKey(int key);
    void handleTagEditorKey(int key);
    void handleLoginKey(int key);
    void handleNeteasePlaylistsKey(int key);
    void handleToplistsKey(int key);

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

    // 网易云相关
    void doQrLogin();
    void doLogout();
    void loadNeteasePlaylists();
    void loadToplists();
    void loadRecommend();
    void openNeteasePlaylist(int idx);
    void openToplist(int idx);
    void refreshAccount();

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
    enum class Page { Player, Library, Playlists, Online, TagEditor, Login, NeteasePlaylists, Toplists };
    Page page = Page::Library;

    // ---- 网易云状态 ----
    NeteaseAccount account;
    std::vector<OnlinePlaylist> npList;   // 我的歌单
    std::vector<OnlinePlaylist> topList;  // 排行榜
    int npCursor = 0;
    int tlCursor = 0;
    bool npDetailMode = false;            // 是否在「歌单内容」里
    std::string npTitle;
    std::vector<OnlineSong> npSongs;
    int npSongCursor = 0;
    int npSongOffset = 0;

    // 登录
    std::string loginQrUrl;
    std::string loginKey;
    int loginCode = 0;
    std::string loginMsg;
    long long loginPollUntilMs = 0;

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
