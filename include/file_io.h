#ifndef FILE_IO_H
#define FILE_IO_H

#include <fstream>
#include <string>
#include <filesystem>

// Windows 下 std::ifstream 用窄字符路径会按 ANSI 代码页解释，
// 导致中文路径打开失败。统一通过 std::filesystem::path 构造，
// 使其走宽字符 API。
namespace fileio {

inline std::ifstream openIn(const std::string& path, std::ios::openmode mode = std::ios::in) {
    return std::ifstream(std::filesystem::path(path), mode);
}

inline std::ofstream openOut(const std::string& path, std::ios::openmode mode = std::ios::out) {
    return std::ofstream(std::filesystem::path(path), mode);
}

} // namespace fileio

#endif
