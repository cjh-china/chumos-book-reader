# 在线阅读支持说明

## 注意事项
由于番茄小说等平台有严格的反爬机制和用户协议限制，**本项目不提供针对特定商业平台的自动爬取破解功能**。

## 推荐做法
1. **使用本地文件**（最简单、最安全）：将小说下载为 TXT 格式后用本阅读器阅读
2. **使用开源爬虫工具**：可以参考以下项目
   - [fanqienovel-downloader](https://github.com/search?q=fanqienovel+downloader) - 搜索相关项目
   - [novel-downloader](https://github.com/search?q=novel+downloader) - 通用小说下载器

## 技术框架
项目已预留接口：
- `HttpClient` - HTTP 请求（需要 libcurl）
- `SiteParser` - 网站解析适配器框架
- 支持通过 URL 参数加载（需要编译时启用 libcurl）

## 安装 libcurl（如需在线功能）
**Windows (vcpkg):**
```bash
vcpkg install curl
cmake .. -DCURL_DIR=[vcpkg路径]/installed/x64-windows
```

**Linux:**
```bash
sudo apt-get install libcurl4-openssl-dev
```

**macOS:**
```bash
brew install curl
```

## so-novel 书源（推荐）

项目已集成 [so-novel](https://github.com/freeok/so-novel) 的本地 WebUI，可在 17 个公开书源间聚合搜索并下载 TXT。

**准备：**

1. 下载 so-novel Windows 版（`sonovel.exe` + `app.jar` + `runtime/`），放到例如 `D:\sonovel-windows`
2. 编辑其 `config.ini`，在 `[web]` 段设置 `enabled = 1`
3. 无需手动启动 —— 阅读器会自动拉起（可在设置里关闭）

**服务地址：** `http://127.0.0.1:7765`，可用环境变量 `SONOVEL_API_BASE` 覆盖。

**使用：**

- 书库 → `/` → `3`，或在阅读界面按 `/` → `3`
- 聚合搜索较慢（10~30 秒），回车下载，章节多的书需要几分钟
- 下载完成后自动复制到 `downloads/` 并直接打开阅读

**查找 sonovel.exe 的顺序：**

1. 设置面板（书库按 `U`）里填写的安装目录
2. 环境变量 `SONOVEL_HOME`
3. 程序同目录及其 `sonovel/`、`tools/`、`tools/sonovel/`
4. 从程序目录向上 4 层找 `sonovel-windows/`、`tools/sonovel/`
5. `D:\sonovel-windows`、`C:\sonovel-windows`
