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
