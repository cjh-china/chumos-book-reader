# 跨平台构建与番茄下载器集成

## 概述

本项目把 [Tomato-Novel-Downloader](https://github.com/zhongbai2333/Tomato-Novel-Downloader) v2.4.16 的
各平台可执行文件**内嵌进 `ebook_reader`**。程序首次使用番茄功能时，会把内嵌的下载器释放到用户可写目录并自动拉起本地服务（`127.0.0.1:18423`），无需手动安装任何东西。

## 目录布局

```
tools/tomato/
├── win64/TomatoNovelDownloader.exe        # Windows x64
├── linux_amd64/TomatoNovelDownloader      # Linux x86_64 (glibc)
├── linux_arm64/TomatoNovelDownloader      # Linux aarch64
├── macos_amd64/TomatoNovelDownloader      # macOS Intel
├── macos_arm64/TomatoNovelDownloader      # macOS Apple Silicon
└── config.yml                             # 默认配置模板（仅首次释放时写入）
```

> 注意：Windows 兼容旧布局，若 `win64/` 不存在则回退读取 `tools/tomato/TomatoNovelDownloader.exe`。

## 内嵌方式

| 平台 | 机制 |
|---|---|
| Windows | `windres` 编译 `resources/fanqie.rc.in` → `RCDATA` 资源，运行时 `FindResource` 读取 |
| Linux / macOS | CMake 调用 `objcopy --input-target binary` 生成 `fanqie_exe.o`，链接后通过 `_binary_TomatoNovelDownloader_start/end` 符号访问 |

构建时若对应平台二进制缺失，会自动跳过内嵌（打印 `embedding skipped`），程序仍可编译，只是运行时需要手动准备下载器。

## 运行时释放位置

| 平台 | 路径 |
|---|---|
| Windows | `%LOCALAPPDATA%\ebook_reader\tomato\` |
| Linux / macOS | `$HOME/.local/share/ebook_reader/tomato/` |
| 任意 | 可用环境变量 `FANQIE_RUNTIME_DIR` 覆盖 |

释放逻辑会校验文件大小，大小一致则跳过覆盖，因此反复启动不会重复写盘。
POSIX 平台释放后自动 `chmod 0755`。

## 构建

### Windows (MSYS2 UCRT64)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH=D:/devtools/msys64/ucrt64
cmake --build build
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install build-essential cmake ninja-build libncurses-dev libcurl4-openssl-dev binutils
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### macOS

```bash
brew install cmake ninja curl
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

> macOS 需要 `binutils` 提供 `objcopy`（`brew install binutils`），或使用系统自带的 `llvm-objcopy`。

## 环境变量

| 变量 | 作用 |
|---|---|
| `FANQIE_API_BASE` | 覆盖服务地址，默认 `http://127.0.0.1:18423` |
| `FANQIE_TOOL_DIR` | 指定下载器所在目录（跳过自动查找） |
| `FANQIE_RUNTIME_DIR` | 指定内嵌资源释放目录 |

## 使用

```bash
./ebook_reader 某本小说.txt
# 阅读界面按 / → 2 → 输入书名 → 上下选择
#   回车 = 下载并直接阅读
#   d    = 下载保存为 txt
```

或纯命令行：

```bash
./ebook_reader --fanqie
```

## 已知限制

- 下载器依赖其内置的第三方 API 池，若上游全部失效则无法取到正文；这不是本项目能控制的。
- 首次搜索需等待服务启动，约 1~2 秒。
- 下载整本耗时取决于章节数与上游限速。
