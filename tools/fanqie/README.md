# 番茄小说 MCP 下载器集成说明

本项目集成了 `fanqie_mcp_downloader.py`，通过 MCP 协议调用 `@fysh925/mcp-server-fanqie` 下载番茄小说。

## 前置条件

1. 安装 Node.js
2. 安装 MCP 服务
```bash
npm install -g @fysh925/mcp-server-fanqie
```

3. 安装 Python 依赖（脚本仅使用标准库，无额外依赖）

## 使用方式

### 方式1：直接运行脚本（最简单）
```bash
cd D:\command-line-ebook-reader\tools\fanqie
python fanqie_mcp_downloader.py
```

按提示搜索书籍、选择并下载。下载完成后会保存在桌面的 `番茄小说下载/` 文件夹中。

### 方式2：从阅读器调用（计划中）
未来将在 TUI 界面中添加"在线下载"选项，调用此脚本。

## 注意事项
- 脚本通过调用外部 MCP 服务获取数据，实际的解析和反爬处理由 MCP 服务负责
- 仅供个人学习和研究使用
- 请遵守相关网站的用户协议和法律法规
