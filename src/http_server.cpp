#include "http_server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// ws2_32 由 CMake 链接（MinGW 不支持 #pragma comment(lib)）
using socklen_t = int;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#endif

namespace api {

// ==================== 字符串工具 ====================

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hexv(s[i + 1]);
            int l = hexv(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        out += (s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::map<std::string, std::string> parseQuery(const std::string& qs) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string pair = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                out[urlDecode(pair)] = "";
            } else {
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// ==================== Response ====================

Response Response::json(const std::string& text, int code) {
    Response r;
    r.status = code;
    r.body = text;
    return r;
}

Response Response::error(const std::string& message, int code) {
    return json("{\"error\":\"" + jsonEscape(message) + "\"}", code);
}

// ==================== Json ====================

void Json::separator() {
    if (expectValue_) {
        expectValue_ = false;
        return;
    }
    if (!firstStack_.empty()) {
        if (!firstStack_.back()) out_ += ',';
        firstStack_.back() = false;
    }
}

Json& Json::beginObject() {
    separator();
    out_ += '{';
    firstStack_.push_back(true);
    return *this;
}

Json& Json::endObject() {
    out_ += '}';
    if (!firstStack_.empty()) firstStack_.pop_back();
    return *this;
}

Json& Json::beginArray() {
    separator();
    out_ += '[';
    firstStack_.push_back(true);
    return *this;
}

Json& Json::endArray() {
    out_ += ']';
    if (!firstStack_.empty()) firstStack_.pop_back();
    return *this;
}

Json& Json::key(const std::string& k) {
    separator();
    out_ += '"';
    out_ += jsonEscape(k);
    out_ += "\":";
    expectValue_ = true;
    return *this;
}

Json& Json::value(const std::string& v) {
    separator();
    out_ += '"';
    out_ += jsonEscape(v);
    out_ += '"';
    return *this;
}

Json& Json::value(const char* v) { return value(std::string(v ? v : "")); }

Json& Json::value(long long v) {
    separator();
    out_ += std::to_string(v);
    return *this;
}

Json& Json::value(int v) { return value((long long)v); }

Json& Json::value(double v) {
    separator();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    // 去掉多余的尾随零
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    out_ += s.empty() ? "0" : s;
    return *this;
}

Json& Json::value(bool v) {
    separator();
    out_ += v ? "true" : "false";
    return *this;
}

Json& Json::raw(const std::string& jsonFragment) {
    separator();
    out_ += jsonFragment;
    return *this;
}

// ==================== Server ====================

struct Server::Impl {
    std::map<std::string, Handler> routes;   // key = "METHOD /path"
    std::atomic<bool> running{false};
#ifdef _WIN32
    SOCKET listenSock = INVALID_SOCKET;
#else
    int listenSock = -1;
#endif

    Response dispatch(const Request& req) {
        auto it = routes.find(req.method + " " + req.path);
        if (it != routes.end()) return it->second(req);

        // 方法不匹配时给出更明确的提示
        for (const auto& kv : routes) {
            if (kv.first.size() > req.path.size() &&
                kv.first.compare(kv.first.size() - req.path.size(), req.path.size(), req.path) == 0) {
                return Response::error("方法不允许: " + req.method + " " + req.path, 405);
            }
        }
        return Response::error("未找到路由: " + req.method + " " + req.path, 404);
    }
};

Server::Server() : impl_(new Impl()) {}

Server::~Server() {
    stop();
    delete impl_;
}

void Server::route(const std::string& method, const std::string& path, Handler handler) {
    impl_->routes[method + " " + path] = std::move(handler);
}

void Server::stop() {
    impl_->running = false;
#ifdef _WIN32
    if (impl_->listenSock != INVALID_SOCKET) {
        CLOSE_SOCKET(impl_->listenSock);
        impl_->listenSock = INVALID_SOCKET;
    }
    WSACleanup();
#else
    if (impl_->listenSock >= 0) {
        CLOSE_SOCKET(impl_->listenSock);
        impl_->listenSock = -1;
    }
#endif
}

namespace {

// 从 socket 读取一行（到 \r\n 为止，不含）
bool readLine(int sock, std::string& line) {
    line.clear();
    char c = 0;
    while (true) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line += c;
        if (line.size() > 8192) return false;   // 防御超长行
    }
}

// 读取恰好 n 字节
bool readExact(int sock, size_t n, std::string& out) {
    out.assign(n, '\0');
    size_t got = 0;
    while (got < n) {
        int r = recv(sock, &out[got], (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

std::string statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

#ifdef _WIN32
void handleClient(SOCKET sock, Server::Impl* impl) {
#else
void handleClient(int sock, Server::Impl* impl) {
#endif
    (void)0;
    std::string requestLine;
    if (!readLine(sock, requestLine)) { CLOSE_SOCKET(sock); return; }

    Request req;
    {
        std::istringstream is(requestLine);
        std::string target;
        std::string version;
        is >> req.method >> target >> version;
        if (req.method.empty() || target.empty()) {
            std::string body = Response::error("请求行格式错误", 400).body;
            std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            send(sock, resp.data(), (int)resp.size(), 0);
            CLOSE_SOCKET(sock);
            return;
        }
        size_t q = target.find('?');
        if (q == std::string::npos) {
            req.path = urlDecode(target);
        } else {
            req.path = urlDecode(target.substr(0, q));
            req.query = parseQuery(target.substr(q + 1));
        }
    }

    // 头
    size_t contentLength = 0;
    while (true) {
        std::string line;
        if (!readLine(sock, line)) break;
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        for (char& ch : k) ch = (char)std::tolower((unsigned char)ch);
        req.headers[k] = v;
        if (k == "content-length") contentLength = (size_t)std::strtoull(v.c_str(), nullptr, 10);
    }

    if (contentLength > 0) {
        if (contentLength > 16 * 1024 * 1024) {
            std::string body = Response::error("请求体过大", 400).body;
            std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            send(sock, resp.data(), (int)resp.size(), 0);
            CLOSE_SOCKET(sock);
            return;
        }
        readExact(sock, contentLength, req.body);
    }

    Response res;
    try {
        res = impl->dispatch(req);
    } catch (const std::exception& e) {
        res = Response::error(std::string("处理请求时异常: ") + e.what(), 500);
    } catch (...) {
        res = Response::error("处理请求时未知异常", 500);
    }

    std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " + statusText(res.status) + "\r\n";
    head += "Content-Type: " + res.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    head += "Connection: close\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "\r\n";

    send(sock, head.data(), (int)head.size(), 0);
    if (!res.body.empty()) {
        send(sock, res.body.data(), (int)res.body.size(), 0);
    }
    CLOSE_SOCKET(sock);
}

} // namespace

bool Server::listen(const std::string& host, int port, std::string& error) {
    error.clear();

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup 失败";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = "创建 socket 失败";
        WSACleanup();
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        error = "端口 " + std::to_string(port) + " 绑定失败（可能已被占用）";
        CLOSE_SOCKET(s);
        WSACleanup();
        return false;
    }
    if (::listen(s, 16) != 0) {
        error = "listen 失败";
        CLOSE_SOCKET(s);
        WSACleanup();
        return false;
    }
    impl_->listenSock = s;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { error = "创建 socket 失败"; return false; }
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        error = "端口 " + std::to_string(port) + " 绑定失败（可能已被占用）";
        CLOSE_SOCKET(s);
        return false;
    }
    if (::listen(s, 16) != 0) {
        error = "listen 失败";
        CLOSE_SOCKET(s);
        return false;
    }
    impl_->listenSock = s;
#endif

    port_ = port;
    impl_->running = true;
    std::printf("[api] listening on http://%s:%d\n", host.c_str(), port);
    std::fflush(stdout);

    // 单线程串行处理：测试用足够，且避免业务层的并发问题
    while (impl_->running) {
#ifdef _WIN32
        SOCKET client = accept(impl_->listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
#else
        int client = accept(impl_->listenSock, nullptr, nullptr);
        if (client < 0) break;
#endif
        handleClient(client, impl_);
    }

    return true;
}

} // namespace api
