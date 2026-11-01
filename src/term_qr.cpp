#include "term_qr.h"
#include "qrcodegen.hpp"

#include <vector>

std::vector<std::string> renderTerminalQr(const std::string& text, int border) {
    std::vector<std::string> out;
    if (text.empty()) return out;

    try {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);

        int size = qr.getSize();
        int full = size + border * 2;

        // 每两个模块合成一行字符：上黑下白=▀，上白下黑=▄，全黑=█，全白=空格
        for (int y = 0; y < full; y += 2) {
            std::string line;
            line.reserve((size_t)full);

            for (int x = 0; x < full; x++) {
                auto get = [&](int yy) -> bool {
                    int gx = x - border;
                    int gy = yy - border;
                    if (gx < 0 || gx >= size || gy < 0 || gy >= size) return false;
                    return qr.getModule(gx, gy);
                };

                bool top = get(y);
                bool bot = (y + 1 < full) ? get(y + 1) : false;

                if (top && bot) line += "\xE2\x96\x88";        // █
                else if (top) line += "\xE2\x96\x80";          // ▀
                else if (bot) line += "\xE2\x96\x84";          // ▄
                else line += " ";
            }
            out.push_back(std::move(line));
        }
    } catch (...) {
        return out;
    }

    return out;
}

int terminalQrWidth(const std::string& text, int border) {
    if (text.empty()) return 0;
    try {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        return qr.getSize() + border * 2;
    } catch (...) {
        return 0;
    }
}
