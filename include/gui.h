#ifndef GUI_H
#define GUI_H

#include <string>

namespace api { class Server; }

namespace gui {

// 注册内嵌阅读器页面（GET / 与 favicon）。仅在 -DENABLE_GUI=ON 时编译。
void registerRoutes(api::Server& server);

// 由 CMake 从 web/reader.html 生成的单页 HTML（C++ raw string，运行时零外部资源）。
const char* readerPage();

// 打开独立的浏览器应用窗口（无地址栏，手机竖屏尺寸）并阻塞到窗口关闭。
// host/port 指向本进程内的 HTTP 服务。找不到 Chromium 时降级为默认浏览器。
bool openWindow(const std::string& host, int port);

} // namespace gui

#endif
