#ifndef FANQIE_RESOURCE_H
#define FANQIE_RESOURCE_H

#include <string>

namespace fanqie_res {

// 本模块支持两种构建模式：
//   1. 外置（默认）：下载器与 config.yml 放在 exe 同目录，仅做定位，不释放任何文件。
//   2. 内嵌（-DEMBED_FANQIE=ON）：下载器编译进 exe，运行时释放到用户可写目录。
// 通过 CMake 选项 EMBED_FANQIE 切换，宏 HAS_EMBEDDED_FANQIE 标识内嵌模式。

// 查找下载器可执行文件；找不到返回空。
// 内嵌模式下若尚未释放，会先释放再返回路径。
std::string locateDownloader();

// 查找 config.yml；找不到返回空。
std::string locateConfig();

// 下载器所在目录；找不到返回空。
// 内嵌模式下返回释放目录。
std::string downloaderDir();

// 检查运行环境：true 表示下载器可用（外置找到，或内嵌释放成功）
bool isAvailable();

// 给用户看的提示（不可用时调用）
std::string missingHint();

#ifdef HAS_EMBEDDED_FANQIE
// 内嵌模式专用：把内嵌资源释放到 targetDir。成功返回 true。
bool extractTo(const std::string& targetDir);

// 内嵌 exe 的字节数（0 表示未内嵌）
size_t embeddedExeSize();

// 判断当前构建是否内嵌了下载器
bool hasEmbedded();
#endif

} // namespace fanqie_res

#endif
