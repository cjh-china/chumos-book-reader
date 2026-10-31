#ifndef TUI_H
#define TUI_H

#include <string>
#include <vector>

class TUI {
public:
    TUI();
    ~TUI();

    bool init();
    void shutdown();

    void clear();
    void refresh();

    int getHeight();
    int getWidth();

    void drawText(int row, int col, const std::string& text);
    void drawLine(int row, int col, const std::string& text, int maxWidth);
    void drawTextColor(int row, int col, const std::string& text, int color);

    int getKey();

    void showStatus(const std::string& status);
    void drawSeparator(int row);

    // 文本输入框：在 (row,col) 显示 prompt 并阻塞读入一行（支持中文/退格/Esc取消）
    std::string promptInput(int row, int col, const std::string& prompt, const std::string& initial = "");
    void setCursorVisible(bool visible);

private:
    bool initialized;
};

#endif
