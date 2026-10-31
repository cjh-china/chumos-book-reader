# 构建指南

本文档说明如何编译本项目，以及各编译选项的作用。

## 目录

- 依赖
- 快速开始
- 编译选项
- 构建组合与体积
- 番茄下载器：外置 vs 内嵌
- 音乐模块
- 本地音乐 API
- 运行时目录
- 常见问题

---

## 依赖

### 通用

| 依赖 | 版本 | 用途 |
|---|---|---|
| CMake | 3.10+ | 构建系统 |
| C++17 编译器 | GCC 9+ / Clang 10+ / MSVC 2019+ | 编译 |
| libcurl | 7.60+ | 在线阅读、番茄下载、在线音乐 |

### 可选依赖（音乐模块）

| 依赖 | 说明 |
|---|---|
| miniaudio | 已内置 third_party/miniaudio.h，无需单独安装 |
| winmm + ole32 (Windows) | 系统自带，CMake 自动链接 |
| iconv (POSIX) | glibc 内置；macOS 需 brew install libiconv |

### 可选依赖（POSIX TUI）

| 依赖 | 说明 |
|---|---|
| ncurses | Linux: libncurses-dev；macOS: 系统自带 |

### Windows（MSYS2 UCRT64）

    pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-curl

### Debian / Ubuntu

    sudo apt install build-essential cmake ninja-build libncurses-dev libcurl4-openssl-dev binutils

### macOS

    brew install cmake ninja curl

---

## 快速开始

### Windows

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/devtools/msys64/ucrt64
    cmake --build build

### Linux / macOS

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

编译完成后运行：

    ./build/ebook_reader.exe      # Windows
    ./build/ebook_reader          # Linux / macOS

---

## 编译选项

| 选项 | 默认 | 说明 |
|---|---|---|
| EMBED_FANQIE | OFF | 是否把番茄下载器内嵌进 exe |
| ENABLE_MUSIC | OFF | 是否编译音乐播放器 |
| ENABLE_LOCAL_MUSIC_API | OFF | 是否接入本地网易云 API（需 ENABLE_MUSIC=ON） |
| LOCAL_MUSIC_API_BASE | http://127.0.0.1:3000 | 本地 API 地址 |
| STATIC_LINK | OFF | Windows 下把所有第三方库静态编入 exe，产物不依赖任何 DLL |

### 依赖关系

    ENABLE_LOCAL_MUSIC_API  ->  需要 ENABLE_MUSIC
    ENABLE_MUSIC            ->  需要 libcurl
    EMBED_FANQIE            ->  需要 tools/tomato/ 下有下载器源文件
    STATIC_LINK             ->  需要 pkg-config 与 libcurl / zlib 的 .a 静态库

依赖不满足时会警告并自动降级，不会导致编译失败。例如：

    CMake Warning:
      ENABLE_LOCAL_MUSIC_API requires ENABLE_MUSIC=ON; ignored
    CMake Warning:
      EMBED_FANQIE=ON but downloader not found; falling back to external

编译时 CMake 会打印当前生效的模式：

    -- Fanqie: EXTERNAL (copied to output dir)
    -- Music player enabled
    -- Local music API enabled: http://127.0.0.1:3000

---

## 构建组合与体积

以下为 Windows x64 Release 实测值（主程序 exe 大小，不含外置下载器）：

| 组合 | 命令 | 主程序 | 配套文件 |
|---|---|---|---|
| 纯阅读器 | 无附加选项 | 0.69 MB | 下载器 10.93 MB |
| 阅读器 + 音乐 | -DENABLE_MUSIC=ON | 1.77 MB | 同上 |
| 全功能 | -DENABLE_MUSIC=ON -DENABLE_LOCAL_MUSIC_API=ON | 1.78 MB | 同上 |
| 单文件（内嵌） | -DEMBED_FANQIE=ON | 11.63 MB | 无 |
| 单文件 + 音乐 | -DEMBED_FANQIE=ON -DENABLE_MUSIC=ON | 约 12.7 MB | 无 |
| 静态链接 | -DSTATIC_LINK=ON | 11.17 MB | 下载器 10.93 MB，无 DLL |

说明：

- 外置模式下 TomatoNovelDownloader.exe 会在编译后自动复制到输出目录
- 内嵌模式下下载器打包进 exe，输出目录只有主程序
- 动态链接（默认）时输出目录还需要 zlib1.dll、libcurl-4.dll 等 MSYS2 运行时；
  加上 -DSTATIC_LINK=ON 后这些全部消失，只剩 Windows 系统 DLL

---

## 静态链接（Windows，无 DLL 依赖）

默认构建会动态链接 MSYS2 的运行时，分发时除了 exe 还要带上：

    zlib1.dll
    libcurl-4.dll 及其依赖（libssl-3-x64.dll、libcrypto-3-x64.dll、
    libssh2-1.dll、libnghttp2-14.dll、libngtcp2-16.dll、libnghttp3-9.dll、
    libpsl-5.dll、libidn2-0.dll、libbrotlidec.dll、libbrotlicommon.dll、
    libzstd.dll、libunistring-5.dll、libiconv-2.dll、
    libgcc_s_seh-1.dll、libstdc++-6.dll、libwinpthread-1.dll）

加 `-DSTATIC_LINK=ON` 后以上 DLL 全部不再需要，只依赖 Windows 自带的
KERNEL32 / USER32 / WS2_32 / CRYPT32 等系统库。

    cmake -S . -B build_static -G Ninja -DCMAKE_BUILD_TYPE=Release ^
          -DCMAKE_PREFIX_PATH=D:/devtools/msys64/ucrt64 -DSTATIC_LINK=ON
    cmake --build build_static

实现要点：

| 项 | 做法 |
|---|---|
| 链接选项 | `-static -static-libgcc -static-libstdc++ -s` |
| libcurl | 用 `pkg-config --libs --static libcurl` 取完整依赖链，再显式换成 `libcurl.a` 的绝对路径 |
| 宏定义 | 追加 `-DCURL_STATICLIB`，否则头文件仍按 dllimport 声明 |
| zlib | 绕过 `find_package(ZLIB)`（它会优先选 `libz.dll.a`），直接链接 `libz.a` |
| 符号表 | `-s` 在链接期 strip，体积从约 15.4 MB 降到 11.2 MB |

注意：静态链接会触发 GPL/LGPL 相关条款（libcurl、OpenSSL 等），
如果打算分发二进制，请自行确认许可证兼容性。

---

## 番茄下载器：外置 vs 内嵌

### 外置模式（默认，推荐）

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

编译后目录结构：

    build/
      ebook_reader.exe            0.69 MB
      TomatoNovelDownloader.exe  10.93 MB   <- 自动复制
      config.yml                            <- 自动复制
      downloads/                            <- 运行时生成
      cache/                                <- 运行时生成

优点：

- 主程序仅 0.69 MB
- 不触发杀软 Dropper 启发式检测（无内嵌 PE、无运行时释放）
- 可随时替换更新版本的下载器

缺点：

- 分发时需连同 TomatoNovelDownloader.exe 一起打包

### 内嵌模式

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEMBED_FANQIE=ON

编译后目录结构：

    build/
      ebook_reader.exe           11.63 MB   <- 单文件
      downloads/
      cache/

运行时首次使用番茄功能会把下载器释放到：

- Windows: %LOCALAPPDATA%\ebook_reader\tomato\
- Linux/macOS: $HOME/.local/share/ebook_reader/tomato/

可用环境变量 FANQIE_RUNTIME_DIR 覆盖释放位置。

优点：

- 单文件分发

缺点：

- 多数杀软会报 HEUR/QVM 或 Trojan.Generic（因为具备 Dropper 的行为特征：内嵌 PE + 运行时释放 + 启动子进程）
- 体积大（+10.9 MB）

建议：除非必须单文件分发，否则用默认的外置模式。

### 查找优先级

两种模式共用同一套查找逻辑，外置优先：

    1. 环境变量 FANQIE_TOOL_DIR
    2. exe 同目录
    3. exe同目录/tomato/
    4. exe同目录/tools/
    5. exe同目录/tools/tomato/
    6. 从 exe 目录向上 4 层找 tools/tomato/
    7. 当前工作目录
    8. ./tools/tomato/

    （以上都找不到，且为内嵌模式时）
    9. 释放内嵌资源到 runtimeDir()

这样用户放的更新版本会优先使用，不会被内嵌的旧版本覆盖。

### 下载器源文件位置

    tools/tomato/
      win64/TomatoNovelDownloader.exe
      linux_amd64/TomatoNovelDownloader
      linux_arm64/TomatoNovelDownloader
      macos_amd64/TomatoNovelDownloader
      macos_arm64/TomatoNovelDownloader
      config.yml

来源：https://github.com/zhongbai2333/Tomato-Novel-Downloader/releases

Linux/macOS 需要自行下载对应平台的二进制放入上述目录，Windows 的已随仓库提供。

---

## 音乐模块

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_MUSIC=ON

### 功能

| 功能 | 说明 |
|---|---|
| 本地曲库 | 扫描目录，读取 ID3v2 / FLAC Vorbis 标签 |
| 播放 | miniaudio，支持 MP3 / FLAC / WAV / OGG |
| 播放模式 | 列表循环 / 随机 / 单曲循环 |
| 歌词 | LRC 解析，支持多时间标签、offset 偏移 |
| 歌单 / 收藏 / 历史 | 持久化到 music_store.json |
| 标签编辑 | 改写 ID3v2.3（保留原有帧，加 1KB padding） |
| 睡眠定时 | 15/30/60 分钟、曲终即停 |
| 在线搜索 | myfreemp3（网易云）、Audius、iTunes |
| 在线下载 | 音频 + 歌词 + 封面，自动进曲库 |

### 在线音源说明

| 音源 | 完整曲目 | 中文歌 | 歌词 | 备注 |
|---|---|---|---|---|
| myfreemp3 | 部分 | 有 | 有 | 无版权的会返回占位音频，程序自动跳过 |
| Audius | 完整 | 无 | 无 | 独立音乐人为主 |
| iTunes | 仅 30 秒 | 全覆盖 | 无 | 试听 |

三者自动合并、按优先级排序。播放失败会自动尝试其他音源的同名曲目。

### 快捷键

音乐模块入口：书库界面按 M。

曲库页：

| 键 | 作用 |
|---|---|
| Up/Down, PgUp/PgDn | 选择 |
| Enter | 播放（当前过滤结果作为播放列表） |
| 空格 | 播放/暂停 |
| / | 过滤 |
| e | 编辑标签 |
| a | 添加扫描目录 |
| r | 重新扫描 |
| f | 收藏 |
| p | 添加到歌单 |
| O | 在线搜索 |
| P | 歌单页 |
| Tab | 播放器页 |
| q | 返回书库 |

播放器页：

| 键 | 作用 |
|---|---|
| 空格 | 播放/暂停 |
| Left/Right | 快退/快进 5 秒 |
| Up/Down | 音量 +/- 5% |
| n / p | 下一首 / 上一首 |
| m | 切换播放模式 |
| l | 显示/隐藏歌词 |
| z | 睡眠定时 |
| O | 在线搜索 |

在线搜索页：

| 键 | 作用 |
|---|---|
| Enter | 播放（先下载到缓存） |
| 空格 / x | 标记/取消，自动下移 |
| a | 全选 / 全不选 |
| d | 下载当前 |
| D | 批量下载已标记 |
| / | 重新搜索 |

---

## 本地音乐 API

额外接入本地的 NeteaseCloudMusicApi，可以拿到网易云全曲库（含无版权歌曲的真实音频）。

### 1. 部署服务

    npm install -g NeteaseCloudMusicApi --registry=https://registry.npmmirror.com
    NeteaseCloudMusicApi

默认监听 http://127.0.0.1:3000。保持窗口开启。

验证服务：

    curl "http://127.0.0.1:3000/search?keywords=test&limit=1"

返回 JSON 且含 result 字段即正常。

### 2. 编译时启用

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_MUSIC=ON -DENABLE_LOCAL_MUSIC_API=ON

自定义服务地址：

    -DLOCAL_MUSIC_API_BASE=http://192.168.1.100:3000

### 3. 行为说明

- netease-local 作为首选音源，排在搜索结果最前
- 服务离线时，首次探测 2 秒后本次运行不再重试，后续搜索无额外延迟
- 服务不可用时自动降级到 myfreemp3 / Audius / iTunes

### 辅助脚本

tools/ 下提供两个启动脚本：

| 脚本 | 用途 |
|---|---|
| 启动音乐服务.bat | 显示窗口，能看到日志，关窗即停 |
| 音乐服务.bat | 静默后台启动 |
| 音乐服务.bat stop | 停止服务 |

服务不会开机自启，需要时手动运行。

---


### 自动更新

| 配置项 | 默认 | 说明 |
|---|---|---|
| `auto_update.books` | `true` | 打开番茄来源的书时，自动检查并下载新章节 |
| `auto_update.downloader` | `true` | 启动时检查 TomatoNovelDownloader 是否有新版本 |

**界面切换**：书库界面按 `U` 打开设置面板，方向键选择，空格/回车切换。

**手动修改**：编辑 exe 同目录的 `ebook_reader.config.json`：

```json
{
  "auto_update": {
    "books": true,
    "downloader": true
  },
  "book_sources": {
    "D:\\books\\某本书.txt": "7603858201947163710"
  }
}
```

`book_sources` 记录 txt 文件与番茄 book_id 的对应关系，由程序在下载时自动写入，无需手动维护。

---

## 运行时目录

程序在 exe 同级创建以下目录：

| 目录 | 内容 |
|---|---|
| downloads/ | 番茄小说下载的 txt |
| downloads/music/ | 在线音乐下载（mp3 + lrc + jpg） |
| cache/ | 在线播放的音频缓存、歌词缓存 |
| ebook_reader.config.json | 书库配置（最近文件、目录、排序） |
| music_store.json | 音乐数据（歌单、收藏、历史） |

书库和曲库会自动收录 downloads/ 和 downloads/music/ 下的文件，无需手动添加扫描目录。

内嵌模式下额外使用：

| 路径 | 内容 |
|---|---|
| %LOCALAPPDATA%\ebook_reader\tomato\ (Windows) | 内嵌下载器的释放位置 |
| $HOME/.local/share/ebook_reader/tomato/ (POSIX) | 同上 |

---

## 常见问题

### 编译时报 libcurl not found

安装 libcurl 开发包，或检查 CMAKE_PREFIX_PATH 是否指向正确的安装位置。

Windows MSYS2：

    pacman -S mingw-w64-ucrt-x86_64-curl
    cmake ... -DCMAKE_PREFIX_PATH=D:/devtools/msys64/ucrt64

未找到时会自动关闭所有在线功能（在线阅读、番茄下载、在线音乐），程序仍可编译和阅读本地文件。

### 中文路径打不开

本项目所有文件 IO 都通过 fsutil（Win32 宽字符 API）和 encoding（GBK/UTF-8 自动识别）处理，理论上不会有中文路径问题。如果遇到，检查：

1. 是否用了最新的源码（早期版本用 std::ifstream 会有问题）
2. 命令行传参是否经过了编码转换（main.cpp 已用 CommandLineToArgvW 处理）

### 杀软报毒

| 报毒名称 | 原因 | 处理 |
|---|---|---|
| HEUR/QVM...Malware.Gen | 启发式打分，内嵌模式释放 PE 文件 | 改用默认的外置模式（EMBED_FANQIE=OFF） |
| Trojan.Generic | 特征匹配，内嵌结构命中通用木马特征 | 同上 |

外置模式下不存在内嵌 PE、不存在运行时释放，上述两类报毒应消失。

如果仍然报毒，检查是否运行了旧版本的 exe（旧版是内嵌的）。

### 音乐没有声音

1. 确认系统音量未静音
2. 检查音频设备是否被其他程序独占
3. 播放器页按 Up 提高音量（程序内音量独立于系统）
4. 看 lastError 提示（播放失败时状态栏会显示）

### 在线搜索无结果

按音源排查：

| 音源 | 可能原因 |
|---|---|
| myfreemp3 | 接口变更或网络问题 |
| Audius | 无中文歌（搜中文基本没结果） |
| iTunes | 需联网，部分地区可能被限 |
| netease-local | 本地服务未启动，或端口不是 3000 |

用 curl 直接测各接口可以快速定位。

### 内嵌模式释放失败

检查：

1. %LOCALAPPDATA% 是否可写
2. 是否被安全软件拦截了文件写入
3. 设置 FANQIE_RUNTIME_DIR 环境变量指定其他目录

---

## 相关文档

- BUILD_WINDOWS.md — Windows 详细构建说明
- BUILD_UNIX.md — Linux/macOS 构建说明
- ONLINE_READING.md — 在线阅读与番茄下载说明
- README.md — 项目总览
