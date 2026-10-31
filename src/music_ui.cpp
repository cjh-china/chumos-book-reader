#include "music_ui.h"
#include "fs_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>

#ifdef PLATFORM_WINDOWS
#include <conio.h>
#include <windows.h>
#else
#include <ncurses.h>
#endif

namespace {
long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}

MusicUI::MusicUI(TUI* t, Player* p, MusicStore* s) : tui(t), player(p), store(s) {}
MusicUI::~MusicUI() {}

int MusicUI::displayWidth(const std::string& s) const {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + len > s.size()) len = 1;
        unsigned int cp = 0;
        if (len == 1) cp = c;
        else if (len == 2) cp = c & 0x1F;
        else if (len == 3) cp = c & 0x0F;
        else cp = c & 0x07;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        bool wide = (cp >= 0x1100 && (
            cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
            (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x20000 && cp <= 0x3FFFD)));
        w += wide ? 2 : 1;
        i += len;
    }
    return w;
}

std::string MusicUI::truncateUtf8(const std::string& s, int maxWidth) const {
    if (maxWidth <= 0) return "";
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + len > s.size()) len = 1;
        unsigned int cp = 0;
        if (len == 1) cp = c;
        else if (len == 2) cp = c & 0x1F;
        else if (len == 3) cp = c & 0x0F;
        else cp = c & 0x07;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        bool wide = (cp >= 0x1100 && (
            cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
            (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x20000 && cp <= 0x3FFFD)));
        int cw = wide ? 2 : 1;
        if (w + cw > maxWidth - 3) {
            return s.substr(0, i) + "...";
        }
        w += cw;
        i += len;
    }
    return s;
}

std::string MusicUI::fmtTime(double sec) const {
    if (sec < 0 || !std::isfinite(sec)) sec = 0;
    int total = (int)sec;
    int m = total / 60;
    int s = total % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

void MusicUI::showMessage(const std::string& msg, int ms) {
    statusMsg = msg;
    statusUntilMs = nowMs() + ms;
}

// ==================== 曲库 ====================

void MusicUI::rescanLibrary() {
    library.clear();
    std::vector<std::string> seen;   // 去重用

    auto addDir = [&](const std::string& d) {
        if (d.empty() || !fsutil::isDirectory(d)) return;

        std::string norm = fsutil::normalize(d);
        for (const auto& s : seen) {
            if (fsutil::normalize(s) == norm) return;
        }
        seen.push_back(d);

        auto songs = musiclib::scanDir(d, 8);
        library.insert(library.end(), songs.begin(), songs.end());
    };

    // 用户手动添加的扫描目录
    for (const auto& d : store->scanDirs()) addDir(d);

    // 在线下载的音乐自动并入（无需手动添加）
    addDir(OnlineMusicService::defaultDownloadDir());

    // 按标题排序
    std::sort(library.begin(), library.end(), [](const Song& a, const Song& b) {
        if (a.title != b.title) return a.title < b.title;
        return a.path < b.path;
    });
    refreshFilter();
    showMessage("扫描完成，共 " + std::to_string(library.size()) + " 首");
}

void MusicUI::refreshFilter() {
    filtered.clear();
    std::string f = filter;
    std::transform(f.begin(), f.end(), f.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    for (size_t i = 0; i < library.size(); i++) {
        if (f.empty()) { filtered.push_back((int)i); continue; }
        std::string hay = library[i].title + " " + library[i].artist + " " + library[i].album;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (hay.find(f) != std::string::npos) filtered.push_back((int)i);
    }
    if (libCursor >= (int)filtered.size()) libCursor = (int)filtered.size() - 1;
    if (libCursor < 0) libCursor = 0;
}

void MusicUI::loadLyricForCurrent() {
    lyric = LyricDoc();
    lyricPath.clear();
    const Song* s = player->current();
    if (!s) return;
    if (s->lyricPath.empty()) return;
    lyric = lrc::parseFile(s->lyricPath);
    lyricPath = s->lyricPath;
}

// ==================== 播放 ====================

void MusicUI::playSelected() {
    if (filtered.empty()) return;
    if (libCursor < 0 || libCursor >= (int)filtered.size()) return;

    // 把当前过滤结果作为播放列表
    std::vector<Song> pl;
    pl.reserve(filtered.size());
    for (int idx : filtered) pl.push_back(library[idx]);

    int start = libCursor;
    player->setPlaylist(pl, start);
    if (player->play(start)) {
        store->recordPlay(pl[start]);
        loadLyricForCurrent();
        page = Page::Player;
        showMessage("正在播放: " + pl[start].title);
    } else {
        showMessage("播放失败: " + player->lastError(), 2000);
    }
}

// ==================== 绘制 ====================

void MusicUI::drawLibrary() {
    int h = tui->getHeight();
    int w = tui->getWidth();

    std::string hdr = " 音乐库 (" + std::to_string(filtered.size()) + "/" +
                      std::to_string(library.size()) + " 首)";
    if (!filter.empty()) hdr += "  过滤: " + filter;
    tui->drawText(0, 0, std::string(w, ' '));
    tui->drawLine(0, 0, hdr, w - 1);

    int listH = h - 2;
    if (libCursor < libOffset) libOffset = libCursor;
    if (libCursor >= libOffset + listH) libOffset = libCursor - listH + 1;
    if (libOffset < 0) libOffset = 0;

    for (int i = 0; i < listH; i++) {
        int row = 1 + i;
        int idx = libOffset + i;
        tui->drawText(row, 0, std::string(w, ' '));

        if (idx >= (int)filtered.size()) continue;

        const Song& s = library[filtered[idx]];
        bool sel = (idx == libCursor);
        bool isCur = (player->current() && player->current()->path == s.path);

        std::string mark = sel ? "> " : "  ";
        if (isCur) mark += (player->isPlaying() && !player->isPaused()) ? "\u25B6 " : "\u2016 ";

        std::string line = mark + s.title + "  -  " + s.artist;
        int avail = w - 4;
        line = truncateUtf8(line, avail);
        tui->drawLine(row, 1, line, avail);

        // 右侧显示时长/大小
        std::string right;
        if (s.durationSec > 0) {
            right = fmtTime(s.durationSec) + " ";
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1fM ", (double)s.fileSize / 1048576.0);
            right = buf;
        }
        int rx = w - displayWidth(right) - 1;
        if (rx > 10) tui->drawText(row, rx, right);
    }

    tui->drawText(h - 1, 0, std::string(w, ' '));
    tui->drawText(h - 1, 0,
        "  ↑↓ 选择 | Enter 播放 | / 过滤 | e 改标签 | a 加目录 | r 重扫 | O 在线 | P 歌单 | Tab 播放器 | q 返回");
}

void MusicUI::drawPlayer() {
    int h = tui->getHeight();
    int w = tui->getWidth();

    const Song* s = player->current();

    tui->drawText(0, 0, std::string(w, ' '));
    tui->drawLine(0, 0, " 正在播放", w - 1);

    // 标题 / 艺术家
    tui->drawText(2, 0, std::string(w, ' '));
    std::string title = s ? s->title : "（未播放）";
    int tw = displayWidth(title);
    tui->drawText(2, std::max(0, (w - tw) / 2), truncateUtf8(title, w - 4));

    tui->drawText(3, 0, std::string(w, ' '));
    std::string artist = s ? s->artist : "";
    if (s && !s->album.empty()) artist += "  ·  " + s->album;
    int aw = displayWidth(artist);
    tui->drawText(3, std::max(0, (w - aw) / 2), truncateUtf8(artist, w - 4));

    // 进度条
    int barRow = 5;
    double pos = player->positionSec();
    double dur = player->durationSec();
    int barW = w - 20;
    if (barW < 10) barW = 10;
    int filled = (dur > 0) ? (int)(pos / dur * barW) : 0;
    if (filled < 0) filled = 0;
    if (filled > barW) filled = barW;

    std::string bar = "[";
    bar += std::string((size_t)filled, '=');
    bar += std::string((size_t)(barW - filled), '-');
    bar += "]";
    tui->drawText(barRow, 0, std::string(w, ' '));
    int barX = std::max(0, (w - barW - 2) / 2);
    tui->drawText(barRow, barX, bar);

    // 时间
    std::string tm = fmtTime(pos) + " / " + fmtTime(dur);
    tui->drawText(barRow + 1, 0, std::string(w, ' '));
    int tmw = displayWidth(tm);
    tui->drawText(barRow + 1, std::max(0, (w - tmw) / 2), tm);

    // 状态
    std::string st = player->isPlaying()
        ? (player->isPaused() ? "暂停" : "播放中")
        : "停止";

    if (player->isDownloading()) {
        double p = player->downloadProgress();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  |  下载中 %.0f%%", p < 0 ? 0.0 : p * 100);
        st += buf;
    }

    if (player->sleepMode() == Player::SleepMode::Minutes) {
        int left = player->sleepRemainSec();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  |  睡眠 %d:%02d", left / 60, left % 60);
        st += buf;
    } else if (player->sleepMode() == Player::SleepMode::EndOfTrack) {
        st += "  |  曲终即停";
    }

    if (player->sleepTriggered()) {
        player->clearSleepTrigger();
        showMessage("睡眠定时已触发，播放已停止", 2500);
    }
    st += "  |  ";
    st += Player::modeName(player->getMode());
    char volbuf[32];
    std::snprintf(volbuf, sizeof(volbuf), "  |  音量 %.0f%%", player->volume() * 100);
    st += volbuf;
    tui->drawText(barRow + 2, 0, std::string(w, ' '));
    int sw = displayWidth(st);
    tui->drawText(barRow + 2, std::max(0, (w - sw) / 2), st);

    // 歌词
    if (showLyric && lyric.valid) {
        int lyRow = barRow + 4;
        int lyH = h - lyRow - 2;
        if (lyH > 2) {
            int cur = lyric.indexAt(pos);
            int start = cur - lyH / 2;
            if (start < 0) start = 0;

            for (int i = 0; i < lyH; i++) {
                int idx = start + i;
                tui->drawText(lyRow + i, 0, std::string(w, ' '));
                if (idx < 0 || idx >= (int)lyric.lines.size()) continue;
                std::string line = lyric.lines[idx].text;
                int lw = displayWidth(line);
                int x = std::max(0, (w - lw) / 2);
                if (idx == cur) {
                    tui->drawLine(lyRow + i, x, "> " + line + " <", w - 2);
                } else {
                    tui->drawLine(lyRow + i, x, line, w - 4);
                }
            }
        }
    }

    tui->drawText(h - 1, 0, std::string(w, ' '));
    std::string hint = "  空格 播放/暂停 | \u2190 \u2192 快退/快进 5s | ↑↓ 音量 | ";
    hint += "n/p 切歌 | m 模式 | l 歌词 | z 定时 | O 在线 | Tab 切页 | q 返回";
    tui->drawLine(h - 1, 0, hint, w - 1);
}

void MusicUI::drawPlaylists() {
    int h = tui->getHeight();
    int w = tui->getWidth();

    tui->drawText(0, 0, std::string(w, ' '));

    if (!inPlaylistDetail) {
        tui->drawLine(0, 0, " 歌单 (" + std::to_string(store->playlists().size()) + " 个)", w - 1);

        int listH = h - 2;
        for (int i = 0; i < listH; i++) {
            int row = 1 + i;
            tui->drawText(row, 0, std::string(w, ' '));
            if (i >= (int)store->playlists().size()) continue;

            const auto& p = store->playlists()[i];
            bool sel = (i == plCursor);
            std::string mark = sel ? "> " : "  ";
            std::string line = mark + p.name + "  (" + std::to_string(p.songPaths.size()) + " 首)";
            tui->drawLine(row, 1, line, w - 3);
        }

        tui->drawText(h - 1, 0, std::string(w, ' '));
        tui->drawText(h - 1, 0,
            "  ↑↓ 选择 | Enter 查看 | n 新建 | r 重命名 | d 删除 | Tab 切页 | q 返回");
    } else {
        if (plCursor < 0 || plCursor >= (int)store->playlists().size()) {
            inPlaylistDetail = false;
            return;
        }
        const auto& p = store->playlists()[plCursor];
        tui->drawLine(0, 0, " 歌单: " + p.name + "  (" + std::to_string(p.songPaths.size()) + " 首)", w - 1);

        int listH = h - 2;
        for (int i = 0; i < listH; i++) {
            int row = 1 + i;
            tui->drawText(row, 0, std::string(w, ' '));
            if (i >= (int)p.songPaths.size()) continue;

            const std::string& sp = p.songPaths[i];
            bool sel = (i == detailCursor);
            std::string mark = sel ? "> " : "  ";
            std::string name = fsutil::stem(sp);
            std::string line = mark + name;
            if (!fsutil::isFile(sp)) line += "  （文件已丢失）";
            tui->drawLine(row, 1, truncateUtf8(line, w - 3), w - 3);
        }

        tui->drawText(h - 1, 0, std::string(w, ' '));
        tui->drawText(h - 1, 0,
            "  ↑↓ 选择 | Enter 播放整个歌单 | d 移除此曲 | x 删除歌单 | Esc 返回");
    }
}

// ==================== 交互 ====================

void MusicUI::showAddDirDialog() {
    tui->clear();
    tui->drawText(2, 2, "输入音乐目录路径（留空取消）：");
    std::string p = tui->promptInput(4, 2, "路径: ");
    if (p.empty()) return;

    if (!fsutil::isDirectory(p)) {
        showMessage("目录不存在: " + p, 1500);
        return;
    }
    store->addScanDir(p);
    rescanLibrary();
}

void MusicUI::showPlaylistPicker() {
    const auto& pls = store->playlists();
    if (pls.empty()) {
        showMessage("还没有歌单，先按 P 去新建", 1500);
        return;
    }
    if (filtered.empty()) return;

    const Song& s = library[filtered[libCursor]];

    tui->clear();
    tui->drawText(1, 2, "添加到歌单：" + truncateUtf8(s.title, 60));
    tui->drawText(2, 2, "----------------------------------------");

    for (size_t i = 0; i < pls.size(); i++) {
        std::string line = "  " + std::to_string(i + 1) + ". " + pls[i].name +
                           "  (" + std::to_string(pls[i].songPaths.size()) + " 首)";
        tui->drawLine(3 + (int)i, 2, line, 70);
    }
    tui->drawText(tui->getHeight() - 1, 0, "  输入编号添加到歌单，回车取消");
    tui->refresh();

    std::string inp = tui->promptInput(tui->getHeight() - 1, 0, "编号: ");
    if (inp.empty()) return;
    int idx = std::atoi(inp.c_str()) - 1;
    if (idx < 0 || idx >= (int)pls.size()) {
        showMessage("无效编号", 1200);
        return;
    }
    if (store->addToPlaylist(idx, {s.path})) {
        showMessage("已添加到 " + pls[idx].name, 1200);
    } else {
        showMessage("已存在，未重复添加", 1200);
    }
}

void MusicUI::showCreatePlaylistDialog() {
    tui->clear();
    tui->drawText(2, 2, "新建歌单，输入名称（留空取消）：");
    std::string name = tui->promptInput(4, 2, "名称: ");
    if (name.empty()) return;
    int idx = store->createPlaylist(name);
    if (idx < 0) {
        showMessage("创建失败（名称重复或为空）", 1500);
    } else {
        showMessage("已创建歌单: " + name, 1200);
        plCursor = idx;
    }
}

void MusicUI::handleLibraryKey(int key) {
    int h = tui->getHeight();

    if (key == 'q' || key == 'Q') { running = false; return; }
    if (key == '\t') { page = Page::Player; return; }
    if (key == 'P') { page = Page::Playlists; inPlaylistDetail = false; return; }
    if (key == 'O') { page = Page::Online; return; }
    if (key == 'e' || key == 'E') { showTagEditor(); return; }
    if (key == 'a' || key == 'A') { showAddDirDialog(); return; }
    if (key == 'r' || key == 'R') { rescanLibrary(); return; }

    if (key == '/') {
        filter = tui->promptInput(h - 1, 0, "过滤: ", filter);
        refreshFilter();
        libCursor = 0;
        libOffset = 0;
        return;
    }

    if (key == 'p' || key == 'P') {
        showPlaylistPicker();
        return;
    }
    if (key == 'f' || key == 'F') {
        if (filtered.empty()) return;
        const Song& s = library[filtered[libCursor]];
        bool added = store->toggleFavorite(s.path);
        showMessage(added ? "已收藏" : "已取消收藏", 900);
        return;
    }

#ifdef PLATFORM_WINDOWS
    if (key == 1000) { if (libCursor > 0) libCursor--; return; }
    if (key == 1001) { if (libCursor + 1 < (int)filtered.size()) libCursor++; return; }
    if (key == 1002) { libCursor -= (h - 2); if (libCursor < 0) libCursor = 0; return; }
    if (key == 1003) { libCursor += (h - 2); if (libCursor >= (int)filtered.size()) libCursor = (int)filtered.size() - 1; return; }
    if (key == 1004) { libCursor = 0; return; }
    if (key == 1005) { libCursor = (int)filtered.size() - 1; if (libCursor < 0) libCursor = 0; return; }
#else
    if (key == KEY_UP) { if (libCursor > 0) libCursor--; return; }
    if (key == KEY_DOWN) { if (libCursor + 1 < (int)filtered.size()) libCursor++; return; }
    if (key == KEY_PPAGE) { libCursor -= (h - 2); if (libCursor < 0) libCursor = 0; return; }
    if (key == KEY_NPAGE) { libCursor += (h - 2); if (libCursor >= (int)filtered.size()) libCursor = (int)filtered.size() - 1; return; }
    if (key == KEY_HOME) { libCursor = 0; return; }
    if (key == KEY_END) { libCursor = (int)filtered.size() - 1; if (libCursor < 0) libCursor = 0; return; }
#endif

    if (key == 13 || key == 10) {
        playSelected();
        return;
    }
    if (key == ' ') {
        player->togglePause();
        return;
    }
}

void MusicUI::handlePlayerKey(int key) {
    if (key == 'q' || key == 'Q') { running = false; return; }
    if (key == '\t') { page = Page::Library; return; }
    if (key == 'O') { page = Page::Online; return; }
    if (key == 'z' || key == 'Z') { showSleepTimerDialog(); return; }

    if (key == ' ') { player->togglePause(); return; }
    if (key == 'n') { player->next(); loadLyricForCurrent(); return; }
    if (key == 'p') { player->prev(); loadLyricForCurrent(); return; }
    if (key == 'm' || key == 'M') {
        PlayMode m = player->getMode();
        if (m == PlayMode::Sequential) m = PlayMode::Shuffle;
        else if (m == PlayMode::Shuffle) m = PlayMode::RepeatOne;
        else m = PlayMode::Sequential;
        player->setMode(m);
        showMessage(std::string("模式: ") + Player::modeName(m));
        return;
    }
    if (key == 'l' || key == 'L') {
        showLyric = !showLyric;
        if (showLyric && !lyric.valid) showMessage("当前歌曲没有歌词文件", 1500);
        return;
    }

#ifdef PLATFORM_WINDOWS
    if (key == 1000) { player->setVolume(player->volume() + 0.05f); return; }
    if (key == 1001) { player->setVolume(player->volume() - 0.05f); return; }
    if (key == 1006) { double p = player->positionSec() - 5; if (p < 0) p = 0; player->seekTo(p); return; }
    if (key == 1007) { player->seekTo(player->positionSec() + 5); return; }
#else
    if (key == KEY_UP) { player->setVolume(player->volume() + 0.05f); return; }
    if (key == KEY_DOWN) { player->setVolume(player->volume() - 0.05f); return; }
    if (key == KEY_LEFT) { double p = player->positionSec() - 5; if (p < 0) p = 0; player->seekTo(p); return; }
    if (key == KEY_RIGHT) { player->seekTo(player->positionSec() + 5); return; }
#endif
}

void MusicUI::handlePlaylistKey(int key) {
    int h = tui->getHeight();

    if (!inPlaylistDetail) {
        if (key == 'q' || key == 'Q') { running = false; return; }
        if (key == '\t') { page = Page::Player; return; }
        if (key == 27) { page = Page::Library; return; }
        if (key == 'n' || key == 'N') { showCreatePlaylistDialog(); return; }

        auto& pls = store->playlists();
        if (plCursor >= (int)pls.size()) plCursor = (int)pls.size() - 1;
        if (plCursor < 0) plCursor = 0;

        if ((key == 'd' || key == 'D') && !pls.empty()) {
            std::string name = pls[plCursor].name;
            store->deletePlaylist(plCursor);
            if (plCursor >= (int)store->playlists().size()) plCursor = (int)store->playlists().size() - 1;
            if (plCursor < 0) plCursor = 0;
            showMessage("已删除歌单: " + name);
            return;
        }
        if ((key == 'r' || key == 'R') && !pls.empty()) {
            std::string cur = pls[plCursor].name;
            std::string name = tui->promptInput(h - 1, 0, "新名称: ", cur);
            if (!name.empty() && name != cur) {
                if (store->renamePlaylist(plCursor, name)) {
                    showMessage("已重命名");
                } else {
                    showMessage("重命名失败（重名）");
                }
            }
            return;
        }

#ifdef PLATFORM_WINDOWS
        if (key == 1000) { if (plCursor > 0) plCursor--; return; }
        if (key == 1001) { if (plCursor + 1 < (int)pls.size()) plCursor++; return; }
#else
        if (key == KEY_UP) { if (plCursor > 0) plCursor--; return; }
        if (key == KEY_DOWN) { if (plCursor + 1 < (int)pls.size()) plCursor++; return; }
#endif
        if (key == 13 || key == 10) {
            if (!pls.empty()) { inPlaylistDetail = true; detailCursor = 0; }
            return;
        }
        return;
    }

    // 歌单详情
    if (key == 27 || key == 'q' || key == 'Q') { inPlaylistDetail = false; return; }

#ifdef PLATFORM_WINDOWS
    if (key == 1000) { if (detailCursor > 0) detailCursor--; return; }
    if (key == 1001) {
        auto& pls = store->playlists();
        if (plCursor >= 0 && plCursor < (int)pls.size()) {
            if (detailCursor + 1 < (int)pls[plCursor].songPaths.size()) detailCursor++;
        }
        return;
    }
#else
    if (key == KEY_UP) { if (detailCursor > 0) detailCursor--; return; }
    if (key == KEY_DOWN) {
        auto& pls = store->playlists();
        if (plCursor >= 0 && plCursor < (int)pls.size()) {
            if (detailCursor + 1 < (int)pls[plCursor].songPaths.size()) detailCursor++;
        }
        return;
    }
#endif
    if (key == 'x' || key == 'X') {
        store->deletePlaylist(plCursor);
        inPlaylistDetail = false;
        if (plCursor >= (int)store->playlists().size()) plCursor = (int)store->playlists().size() - 1;
        if (plCursor < 0) plCursor = 0;
        showMessage("已删除歌单");
        return;
    }
    if ((key == 'd' || key == 'D') && plCursor >= 0 && plCursor < (int)store->playlists().size()) {
        auto& pls = store->playlists();
        auto& v = pls[plCursor].songPaths;
        if (detailCursor >= 0 && detailCursor < (int)v.size()) {
            store->removeFromPlaylist(plCursor, v[detailCursor]);
            if (detailCursor >= (int)store->playlists()[plCursor].songPaths.size()) {
                detailCursor = (int)store->playlists()[plCursor].songPaths.size() - 1;
                if (detailCursor < 0) detailCursor = 0;
            }
            showMessage("已从歌单移除");
        }
        return;
    }
    if (key == 13 || key == 10) {
        auto& pls = store->playlists();
        if (plCursor >= 0 && plCursor < (int)pls.size()) {
            auto& v = pls[plCursor].songPaths;
            if (!v.empty() && detailCursor < (int)v.size()) {
                std::vector<Song> songs;
                std::vector<int> valid;
                for (size_t i = 0; i < v.size(); i++) {
                    if (!fsutil::isFile(v[i])) continue;
                    songs.push_back(musiclib::readMetadata(v[i]));
                    valid.push_back((int)i);
                }
                if (songs.empty()) { showMessage("歌单里的文件都不存在", 1500); return; }

                int start = 0;
                for (size_t i = 0; i < valid.size(); i++) {
                    if (valid[i] == detailCursor) { start = (int)i; break; }
                }
                player->setPlaylist(songs, start);
                if (player->play(start)) {
                    store->recordPlay(songs[start]);
                    loadLyricForCurrent();
                    page = Page::Player;
                } else {
                    showMessage("播放失败: " + player->lastError(), 2000);
                }
            }
        }
        return;
    }
}


// ==================== 在线搜索 ====================

void MusicUI::doOnlineSearch() {
    int h = tui->getHeight();
    tui->clear();
    tui->drawText(2, 2, "在线搜索（回车搜索，留空取消）");
    std::string srcList;
    for (const auto& s : OnlineMusicService::availableSources()) {
        if (!srcList.empty()) srcList += " + ";
        srcList += s;
    }
    tui->drawText(3, 2, "音源: " + srcList);
    std::string kw = tui->promptInput(5, 2, "关键词: ", onlineQuery);
    if (kw.empty()) return;

    onlineQuery = kw;
    tui->clear();
    tui->drawText(2, 2, "正在搜索「" + kw + "」，请稍候...");
    tui->refresh();

    onlineResults = online.search(kw);
    onlineMarked.assign(onlineResults.size(), false);
    onlineCursor = 0;
    onlineOffset = 0;

    if (onlineResults.empty()) {
        showMessage("没有结果: " + online.lastError(), 2000);
    } else {
        showMessage("找到 " + std::to_string(onlineResults.size()) + " 条", 1200);
    }
}

void MusicUI::drawOnline() {
    int h = tui->getHeight();
    int w = tui->getWidth();

    std::string hdr = " 在线音乐";
    if (!onlineQuery.empty()) hdr += "  [" + onlineQuery + "]";
    hdr += "  (" + std::to_string(onlineResults.size()) + " 条)";

    int markedCount = 0;
    for (bool b : onlineMarked) if (b) markedCount++;
    if (markedCount > 0) hdr += "  已选 " + std::to_string(markedCount) + " 首";

    if (!onlineSource.empty()) hdr += "  音源:" + onlineSource;
    tui->drawText(0, 0, std::string(w, ' '));
    tui->drawLine(0, 0, hdr, w - 1);

    if (onlineResults.empty()) {
        tui->drawText(2, 2, "还没有搜索结果。按 / 输入关键词搜索。");
        tui->drawText(4, 2, "Audius 提供完整曲目（独立音乐人为主）");
        tui->drawText(5, 2, "iTunes 提供主流歌曲的 30 秒试听");
        tui->drawText(h - 1, 0, std::string(w, ' '));
        tui->drawText(h - 1, 0, "  / 搜索 | Tab 切页 | q 返回");
        return;
    }

    int listH = h - 2;
    if (onlineCursor < onlineOffset) onlineOffset = onlineCursor;
    if (onlineCursor >= onlineOffset + listH) onlineOffset = onlineCursor - listH + 1;
    if (onlineOffset < 0) onlineOffset = 0;

    for (int i = 0; i < listH; i++) {
        int row = 1 + i;
        int idx = onlineOffset + i;
        tui->drawText(row, 0, std::string(w, ' '));
        if (idx >= (int)onlineResults.size()) continue;

        const OnlineSong& s = onlineResults[idx];
        bool sel = (idx == onlineCursor);

        std::string mark = sel ? "> " : "  ";
        bool mk = (idx < (int)onlineMarked.size()) && onlineMarked[idx];
        mark += mk ? "[x] " : "[ ] ";

        std::string src = "[" + s.source + "] ";
        if (s.previewOnly) src = "[试听] ";

        std::string line = mark + src + s.title + "  -  " + s.artist;
        line = truncateUtf8(line, w - 16);
        tui->drawLine(row, 1, line, w - 16);

        // 右侧时长
        std::string right = fmtTime(s.durationSec) + " ";
        int rx = w - displayWidth(right) - 1;
        if (rx > 10) tui->drawText(row, rx, right);
    }

    tui->drawText(h - 1, 0, std::string(w, ' '));
    std::string hint = "  ↑↓ 选择 | Enter 播放 | 空格/x 标记 | d 下载当前";
    if (markedCount > 0) hint += " | D 下载已选(" + std::to_string(markedCount) + ")";
    hint += " | / 搜索 | Tab 切页 | q 返回";
    tui->drawText(h - 1, 0, hint);
}

void MusicUI::playOnlineSelected() {
    if (onlineResults.empty()) return;
    if (onlineCursor < 0 || onlineCursor >= (int)onlineResults.size()) return;

    const OnlineSong& s = onlineResults[onlineCursor];

    // 若音源带歌词，先落到缓存目录，播放后 lyric 加载可自动命中
    std::string lrcCachePath;
    if (!s.lyricText.empty()) {
        std::string base = fsutil::exeDir();
        if (base.empty()) base = ".";
        std::string lrcDir = fsutil::join(base, "cache");
        fsutil::createDirs(lrcDir);

        std::hash<std::string> hasher;
        char nameBuf[64];
        std::snprintf(nameBuf, sizeof(nameBuf), "lrc_%zx.lrc", hasher(s.id + s.title));
        lrcCachePath = fsutil::join(lrcDir, nameBuf);

        if (!fsutil::isFile(lrcCachePath)) {
            fsutil::writeAll(lrcCachePath, s.lyricText);
        }
    }

    int h = tui->getHeight();
    int w = tui->getWidth();

    tui->clear();
    tui->drawText(2, 2, "正在获取音频流...");
    tui->drawText(4, 2, truncateUtf8(s.title, w - 6));
    tui->drawText(5, 2, truncateUtf8(s.artist, w - 6));
    if (s.previewOnly) {
        tui->drawText(7, 2, "注意：iTunes 音源只有 30 秒试听片段");
    }
    tui->refresh();

    std::string url = online.resolveStream(s);
    if (url.empty()) {
        // 把底层原因（无版权 / 请求失败 / 网络错误）带给用户，而不是笼统一句"无法获取"
        std::string why = online.lastError();
        showMessage(why.empty() ? "无法获取播放地址" : why, 3000);
        return;
    }

    if (player->playUrl(url, s.title, s.artist)) {
        // 挂上在线歌词
        lyric = LyricDoc();
        lyricPath.clear();
        if (!lrcCachePath.empty() && fsutil::isFile(lrcCachePath)) {
            lyric = lrc::parseFile(lrcCachePath);
            lyricPath = lrcCachePath;
        }
        showLyric = lyric.valid;

        page = Page::Player;
        showMessage("正在播放: " + s.title + (lyric.valid ? "（含歌词）" : ""), 1500);
        return;
    }

    // 播放失败（多半是无版权占位音频），自动尝试下一条同名结果
    std::string firstErr = "播放失败：" + player->lastError();
    std::string wantTitle = s.title;

    for (int i = 0; i < (int)onlineResults.size(); i++) {
        if (i == onlineCursor) continue;
        const OnlineSong& alt = onlineResults[i];
        if (alt.title != wantTitle) continue;
        if (alt.streamUrl.empty()) continue;

        tui->clear();
        tui->drawText(2, 2, "尝试其他音源的同名曲目...");
        tui->drawText(4, 2, truncateUtf8(alt.title + " - " + alt.artist, w - 6));
        tui->drawText(5, 2, "来源: " + alt.source);
        tui->refresh();

        if (player->playUrl(alt.streamUrl, alt.title, alt.artist)) {
            onlineCursor = i;

            lyric = LyricDoc();
            lyricPath.clear();
            showLyric = false;
            if (!alt.lyricText.empty()) {
                std::string base = fsutil::exeDir();
                if (base.empty()) base = ".";
                std::string lrcDir = fsutil::join(base, "cache");
                fsutil::createDirs(lrcDir);

                std::hash<std::string> hasher;
                char nb[64];
                std::snprintf(nb, sizeof(nb), "lrc_%zx.lrc", hasher(alt.id + alt.title));
                std::string lp = fsutil::join(lrcDir, nb);
                if (!fsutil::isFile(lp)) fsutil::writeAll(lp, alt.lyricText);
                if (fsutil::isFile(lp)) {
                    lyric = lrc::parseFile(lp);
                    lyricPath = lp;
                    showLyric = lyric.valid;
                }
            }

            page = Page::Player;
            showMessage("已切换音源: " + alt.source, 1500);
            return;
        }
    }

    showMessage(firstErr, 4000);
}

void MusicUI::handleOnlineKey(int key) {
    int h = tui->getHeight();

    if (key == 'q' || key == 'Q') { running = false; return; }
    if (key == '\t') { page = Page::Player; return; }
    if (key == 27) { page = Page::Library; return; }
    if (key == '/') { doOnlineSearch(); return; }

#ifdef PLATFORM_WINDOWS
    if (key == 1000) { if (onlineCursor > 0) onlineCursor--; return; }
    if (key == 1001) { if (onlineCursor + 1 < (int)onlineResults.size()) onlineCursor++; return; }
    if (key == 1002) { onlineCursor -= (h - 2); if (onlineCursor < 0) onlineCursor = 0; return; }
    if (key == 1003) { onlineCursor += (h - 2); if (onlineCursor >= (int)onlineResults.size()) onlineCursor = (int)onlineResults.size() - 1; return; }
#else
    if (key == KEY_UP) { if (onlineCursor > 0) onlineCursor--; return; }
    if (key == KEY_DOWN) { if (onlineCursor + 1 < (int)onlineResults.size()) onlineCursor++; return; }
    if (key == KEY_PPAGE) { onlineCursor -= (h - 2); if (onlineCursor < 0) onlineCursor = 0; return; }
    if (key == KEY_NPAGE) { onlineCursor += (h - 2); if (onlineCursor >= (int)onlineResults.size()) onlineCursor = (int)onlineResults.size() - 1; return; }
#endif

    if (key == 13 || key == 10) { playOnlineSelected(); return; }

    // 空格或 x：标记
    if (key == ' ' || key == 'x' || key == 'X') {
        if (onlineCursor >= 0 && onlineCursor < (int)onlineMarked.size()) {
            onlineMarked[onlineCursor] = !onlineMarked[onlineCursor];
            if (onlineCursor + 1 < (int)onlineResults.size()) onlineCursor++;
        }
        return;
    }

    // d：下载当前
    if (key == 'd') { downloadOnlineSelected(); return; }

    // D：下载所有已标记
    if (key == 'D') { downloadOnlineMarked(); return; }

    // a：全选/全不选
    if (key == 'a' || key == 'A') {
        int cnt = 0;
        for (bool b : onlineMarked) if (b) cnt++;
        bool all = (cnt == (int)onlineMarked.size() && !onlineMarked.empty());
        for (size_t i = 0; i < onlineMarked.size(); i++) onlineMarked[i] = !all;
        return;
    }
}



// ==================== 在线下载 ====================

void MusicUI::downloadOnlineSelected() {
    if (onlineResults.empty()) return;
    if (onlineCursor < 0 || onlineCursor >= (int)onlineResults.size()) return;

    const OnlineSong& s = onlineResults[onlineCursor];

    int h = tui->getHeight();
    int w = tui->getWidth();

    tui->clear();
    tui->drawText(2, 2, "正在下载...");
    tui->drawText(4, 2, truncateUtf8(s.title, w - 6));
    tui->drawText(5, 2, truncateUtf8(s.artist, w - 6));
    tui->drawText(7, 2, "保存到: " + OnlineMusicService::defaultDownloadDir());
    tui->refresh();

    std::string err;
    std::string path = online.downloadSong(s, "", err);

    tui->clear();
    if (path.empty()) {
        tui->drawText(2, 2, "下载失败");
        tui->drawText(4, 2, err);
    } else {
        tui->drawText(2, 2, "下载完成");
        tui->drawText(4, 2, truncateUtf8(fsutil::filename(path), w - 6));
        tui->drawText(6, 2, "保存位置:");
        tui->drawText(7, 2, truncateUtf8(OnlineMusicService::defaultDownloadDir(), w - 6));

        // err 非空表示音频已下载，但元数据（歌词/封面）有缺失或写入失败
        int row = 9;
        if (!err.empty()) {
            tui->drawText(row++, 2, "元数据提示:");
            std::string rest = err;
            while (!rest.empty() && row < h - 2) {
                size_t cut = rest.find(';');
                std::string one = (cut == std::string::npos) ? rest : rest.substr(0, cut);
                rest = (cut == std::string::npos) ? "" : rest.substr(cut + 1);
                while (!one.empty() && one.front() == ' ') one.erase(one.begin());
                if (one.empty()) continue;
                tui->drawLine(row++, 4, "· " + truncateUtf8(one, w - 8), w - 8);
            }
            row++;
        }
        tui->drawText(row < h - 1 ? row : h - 2, 2, "按 r 重新扫描曲库即可看到");
    }
    tui->drawText(h - 1, 0, "  按任意键返回");
    tui->refresh();
    tui->getKey();

    if (!path.empty()) {
        rescanLibrary();   // 立即让新歌出现在曲库
        showMessage("已保存: " + fsutil::filename(path), 2000);
    }
}

void MusicUI::downloadOnlineMarked() {
    if (onlineResults.empty()) return;

    std::vector<int> targets;
    for (size_t i = 0; i < onlineMarked.size() && i < onlineResults.size(); i++) {
        if (onlineMarked[i]) targets.push_back((int)i);
    }

    if (targets.empty()) {
        showMessage("没有标记任何曲目（空格/x 标记）", 1500);
        return;
    }

    int h = tui->getHeight();
    int w = tui->getWidth();

    int okCount = 0;
    int failCount = 0;
    std::vector<std::string> failed;

    for (size_t t = 0; t < targets.size(); t++) {
        const OnlineSong& s = onlineResults[targets[t]];

        tui->clear();
        tui->drawText(0, 0, " 批量下载 (" + std::to_string(t + 1) + "/" +
                            std::to_string(targets.size()) + ")");
        for (int i = 0; i < w; i++) tui->drawText(1, i, " ");

        tui->drawText(2, 2, truncateUtf8(s.title, w - 6));
        tui->drawText(3, 2, truncateUtf8(s.artist, w - 6));
        tui->drawText(5, 2, "已成功: " + std::to_string(okCount));
        tui->drawText(6, 2, "已失败: " + std::to_string(failCount));

        int barW = w - 8;
        if (barW > 0) {
            int filled = (int)((double)t / (double)targets.size() * (double)barW);
            std::string bar = "[";
            bar += std::string((size_t)filled, '#');
            bar += std::string((size_t)(barW - filled), '.');
            bar += "]";
            tui->drawText(8, 2, bar);
        }

        tui->drawText(h - 1, 0, std::string(w, ' '));
        tui->drawText(h - 1, 0, "  下载中...");
        tui->refresh();

        std::string err;
        std::string path = online.downloadSong(s, "", err);
        if (!path.empty()) {
            okCount++;
            onlineMarked[targets[t]] = false;
        } else {
            failCount++;
            failed.push_back(s.title + " (" + err + ")");
        }
    }

    // 汇总
    tui->clear();
    tui->drawText(0, 0, " 批量下载完成");
    tui->drawText(2, 2, "成功: " + std::to_string(okCount) + " 首");
    if (failCount > 0) {
        tui->drawText(3, 2, "失败: " + std::to_string(failCount) + " 首");
        int row = 5;
        for (size_t i = 0; i < failed.size() && row < h - 2; i++, row++) {
            tui->drawLine(row, 2, "  - " + failed[i], w - 4);
        }
    }
    tui->drawText(h - 1, 0, std::string(w, ' '));
    tui->drawText(h - 1, 0, "  按任意键返回列表");
    tui->refresh();
    tui->getKey();

    if (okCount > 0) rescanLibrary();
}

// ==================== 标签编辑 ====================

void MusicUI::showTagEditor() {
    if (filtered.empty()) return;
    if (libCursor < 0 || libCursor >= (int)filtered.size()) return;

    tagSong = library[filtered[libCursor]];
    tagField = 0;
    tagMsg.clear();
    page = Page::TagEditor;
}

void MusicUI::drawTagEditor() {
    int h = tui->getHeight();
    int w = tui->getWidth();

    tui->drawText(0, 0, std::string(w, ' '));
    tui->drawLine(0, 0, " 编辑标签", w - 1);

    tui->drawText(2, 0, std::string(w, ' '));
    tui->drawLine(2, 2, "文件: " + truncateUtf8(fsutil::filename(tagSong.path), w - 10), w - 4);

    const char* labels[3] = {"标题", "艺术家", "专辑"};
    const std::string* values[3] = {&tagSong.title, &tagSong.artist, &tagSong.album};

    for (int i = 0; i < 3; i++) {
        int row = 4 + i * 2;
        tui->drawText(row, 0, std::string(w, ' '));
        bool sel = (i == tagField);
        std::string mark = sel ? "> " : "  ";
        std::string line = mark + labels[i] + ": " + *values[i];
        tui->drawLine(row, 2, truncateUtf8(line, w - 6), w - 6);
    }

    if (!tagMsg.empty()) {
        tui->drawText(11, 0, std::string(w, ' '));
        tui->drawLine(11, 2, tagMsg, w - 4);
    }

    tui->drawText(h - 1, 0, std::string(w, ' '));
    tui->drawText(h - 1, 0, "  ↑↓ 切换字段 | Enter 编辑 | s 保存 | Esc 取消");
}

void MusicUI::handleTagEditorKey(int key) {
    int h = tui->getHeight();
    int w = tui->getWidth();

    if (key == 27) { page = Page::Library; return; }
    if (key == 'q' || key == 'Q') { running = false; return; }

    // 字段切换
#ifdef PLATFORM_WINDOWS
    if (key == 1000) { tagField = (tagField + 2) % 3; return; }
    if (key == 1001) { tagField = (tagField + 1) % 3; return; }
#else
    if (key == KEY_UP) { tagField = (tagField + 2) % 3; return; }
    if (key == KEY_DOWN) { tagField = (tagField + 1) % 3; return; }
#endif

    // 编辑当前字段
    if (key == 13 || key == 10) {
        std::string cur;
        const char* label = "";
        if (tagField == 0) { cur = tagSong.title; label = "标题: "; }
        else if (tagField == 1) { cur = tagSong.artist; label = "艺术家: "; }
        else { cur = tagSong.album; label = "专辑: "; }

        std::string v = tui->promptInput(h - 2, 0, label, cur);
        if (tagField == 0) tagSong.title = v;
        else if (tagField == 1) tagSong.artist = v;
        else tagSong.album = v;
        return;
    }

    // 保存
    if (key == 's' || key == 'S') {
        std::string err;
        bool ok = musiclib::writeTags(tagSong.path, tagSong.title,
                                      tagSong.artist, tagSong.album, err);
        if (ok) {
            tagMsg = "已保存";
            // 更新内存里的曲库
            for (auto& s : library) {
                if (s.path == tagSong.path) {
                    s.title = tagSong.title;
                    s.artist = tagSong.artist;
                    s.album = tagSong.album;
                    break;
                }
            }
            refreshFilter();
            showMessage("标签已保存", 1500);
        } else {
            tagMsg = "保存失败: " + err;
            showMessage(tagMsg, 3000);
        }
        return;
    }
}

// ==================== 睡眠定时 ====================

void MusicUI::showSleepTimerDialog() {
    int h = tui->getHeight();
    int w = tui->getWidth();

    tui->clear();
    tui->drawText(1, 2, "睡眠定时");
    tui->drawText(2, 2, "----------------------------------------");
    tui->drawText(4, 2, "  1. 15 分钟后停止");
    tui->drawText(5, 2, "  2. 30 分钟后停止");
    tui->drawText(6, 2, "  3. 60 分钟后停止");
    tui->drawText(7, 2, "  4. 本首歌结束后停止");
    tui->drawText(8, 2, "  0. 取消定时");

    std::string cur = "当前: ";
    if (player->sleepMode() == Player::SleepMode::Minutes) {
        int left = player->sleepRemainSec();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "还有 %d 分 %d 秒", left / 60, left % 60);
        cur += buf;
    } else if (player->sleepMode() == Player::SleepMode::EndOfTrack) {
        cur += "本曲结束后停止";
    } else {
        cur += "未设置";
    }
    tui->drawText(10, 2, cur);

    tui->drawText(h - 1, 0, "  输入选项 (0-4)，回车取消");
    tui->refresh();

    std::string inp = tui->promptInput(h - 1, 0, "选项: ");
    if (inp.empty()) return;

    int opt = std::atoi(inp.c_str());
    switch (opt) {
        case 0: player->setSleepTimer(0); showMessage("已取消睡眠定时", 1200); break;
        case 1: player->setSleepTimer(15); showMessage("15 分钟后停止", 1200); break;
        case 2: player->setSleepTimer(30); showMessage("30 分钟后停止", 1200); break;
        case 3: player->setSleepTimer(60); showMessage("60 分钟后停止", 1200); break;
        case 4: player->setSleepEndOfTrack(true); showMessage("本曲结束后停止", 1200); break;
        default: showMessage("无效选项", 1000); break;
    }
}

// ==================== 主循环 ====================

void MusicUI::run() {
    running = true;

    // 首次进入时若还没有扫描目录，提示添加
    if (store->scanDirs().empty()) {
        rescanLibrary();
        tui->clear();
        tui->drawText(2, 2, "音乐库为空，尚未添加任何扫描目录。");
        tui->drawText(4, 2, "按任意键后按 a 添加音乐目录。");
        tui->refresh();
        tui->getKey();
    } else {
        rescanLibrary();
    }

    bool needRedraw = true;

    while (running) {
        if (needRedraw) {
            switch (page) {
                case Page::Library: drawLibrary(); break;
                case Page::Player: drawPlayer(); break;
                case Page::Playlists: drawPlaylists(); break;
                case Page::Online: drawOnline(); break;
                case Page::TagEditor: drawTagEditor(); break;
            }

            if (!statusMsg.empty() && nowMs() < statusUntilMs) {
                int h = tui->getHeight();
                int w = tui->getWidth();
                tui->drawText(h - 2, 0, std::string(w, ' '));
                tui->drawLine(h - 2, 1, statusMsg, w - 2);
            } else if (!statusMsg.empty()) {
                statusMsg.clear();
            }

            tui->refresh();
            needRedraw = false;
        }

        player->tick();

        int key = tui->getKey();
        if (key != -1) {
            switch (page) {
                case Page::Library: handleLibraryKey(key); break;
                case Page::Player: handlePlayerKey(key); break;
                case Page::Playlists: handlePlaylistKey(key); break;
                case Page::Online: handleOnlineKey(key); break;
                case Page::TagEditor: handleTagEditorKey(key); break;
            }
            needRedraw = true;
            continue;
        }

        // 播放进度需要持续刷新
        if (page == Page::Player && player->isPlaying() && !player->isPaused()) {
            needRedraw = true;
        }

#ifdef PLATFORM_WINDOWS
        Sleep(100);
#else
        napms(100);
#endif
    }

    // 退出音乐模块不停止播放（后台继续）
}
