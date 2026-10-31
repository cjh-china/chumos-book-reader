#ifndef API_ROUTES_H
#define API_ROUTES_H

#include "http_server.h"

namespace api {

// 注册所有 API 端点。
// 每个端点只依赖无 UI 的业务层（Parser / Config / FanqieDownloader /
// SoNovel / musiclib / OnlineMusicService / Player），不触碰 TUI。
void registerRoutes(Server& server);

} // namespace api

#endif
