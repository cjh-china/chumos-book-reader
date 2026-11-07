// ==================== 书签与进度 ====================

#include "reader.h"

#include <string>

// saveProgress() 的实现在 reader.cpp，此处只保留书签 UI 占位实现。

void Reader::toggleBookmark() {
    if (book.wrappedContent.empty()) return;
    // 简化实现：显示书签菜单
    showBookmarkMenu();
}

void Reader::showBookmarks() {
    tui->clear();
    int height = tui->getHeight();
    int width = tui->getWidth();

    tui->drawText(0, 0, " 书签列表");
    for (int i = 0; i < width - 4; i++) tui->drawText(0, 4 + i, " ");

    // 这里需要文件路径来获取书签
    // 暂时显示占位信息
    tui->drawText(2, 2, "书签功能完善中...");

    tui->drawText(height - 1, 0, std::string(width, ' '));
    tui->drawText(height - 1, 0, "  按任意键返回");
    tui->refresh();
    tui->getKey();
}

void Reader::showBookmarkMenu() {
    tui->clear();
    int height = tui->getHeight();
    int width = tui->getWidth();

    tui->drawText(0, 0, " 书签操作");
    for (int i = 0; i < width - 4; i++) tui->drawText(0, 4 + i, " ");

    tui->drawText(2, 2, "m/M - 在当前位置添加/切换书签");
    tui->drawText(3, 2, "B - 查看所有书签");
    tui->drawText(4, 2, "书签将自动保存到配置文件");

    tui->drawText(height - 1, 0, std::string(width, ' '));
    tui->drawText(height - 1, 0, "  按任意键返回");
    tui->refresh();
    tui->getKey();
}