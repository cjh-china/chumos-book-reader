#ifndef FS_UTIL_H
#define FS_UTIL_H

#include <string>
#include <vector>

// UTF-8 安全的文件系统工具。
// Windows 下全部走宽字符 Win32 API，避免 std::filesystem 在 MinGW 上
// 对含中文的 UTF-8 路径抛 "Illegal byte sequence"。
namespace fsutil {

// 路径拼接（纯字符串，自动处理分隔符）
std::string join(const std::string& a, const std::string& b);
std::string parent(const std::string& path);
std::string filename(const std::string& path);
std::string stem(const std::string& path);       // 去掉扩展名的文件名
std::string extension(const std::string& path);  // 小写扩展名，不含点
std::string normalize(const std::string& path);  // 去掉多余分隔符与尾部斜杠

// 当前可执行文件所在目录（UTF-8）；失败返回空
std::string exeDir();

bool exists(const std::string& path);
bool isDirectory(const std::string& path);
bool isFile(const std::string& path);
long long fileSize(const std::string& path);     // 失败返回 -1
long long lastWriteMs(const std::string& path);  // 失败返回 0

// 读整个文件；成功返回 true
bool readAll(const std::string& path, std::string& out);

// 只读文件前 maxBytes 字节（用于读标签头，避免加载整个音频）
bool readHead(const std::string& path, size_t maxBytes, std::string& out);
// 写整个文件（覆盖）；成功返回 true
bool writeAll(const std::string& path, const std::string& data);
// 原子替换：先写 .tmp 再改名
bool writeAllAtomic(const std::string& path, const std::string& data);

bool removeFile(const std::string& path);
bool createDirs(const std::string& path);

// 递归列出目录下扩展名匹配的文件（UTF-8 绝对路径）
std::vector<std::string> listBooksRecursive(const std::string& dir, int maxDepth);

// 递归列出目录下所有文件（UTF-8 绝对路径），跳过隐藏目录
std::vector<std::string> listFilesRecursive(const std::string& dir, int maxDepth);

} // namespace fsutil

#endif
