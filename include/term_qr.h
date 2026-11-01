#ifndef TERM_QR_H
#define TERM_QR_H

#include <string>
#include <vector>

// 用 Unicode 半块字符在终端绘制二维码（每个字符表示 2 行像素，宽高比正确）
// 返回每一行的字符串，调用方逐行绘制。
// 边框默认 4 个模块（静止区），可用 border 参数调整。
// 出错（内容过长）返回空 vector。
std::vector<std::string> renderTerminalQr(const std::string& text, int border = 2);

// 计算终端二维码占用的字符列宽（不含边框时是模块数，含边框每侧 border 个模块）
int terminalQrWidth(const std::string& text, int border = 2);

#endif
