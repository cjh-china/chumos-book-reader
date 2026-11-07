#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <functional>
#include <map>
#include <string>
#include <vector>

// 极简 HTTP/1.1 服务器，仅用于本地 API 测试。
// 特性：GET/POST、按方法+路径路由、查询参数解析、JSON 请求/响应。
// 非目标：并发性能、keep-alive、分块传输、TLS。
namespace api {

struct Request {
    std::string method;
    std::string path;                          // 不含查询串
    std::map<std::string, std::string> query;  // 已 URL 解码
    std::map<std::string, std::string> headers;  // key 小写
    std::string body;
};

struct Response {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    // 额外响应头（音频文件的 Range / Content-Range 等）；键按原样输出。
    std::map<std::string, std::string> headers;

    // 便捷方法：返回 JSON 字符串响应
    static Response json(const std::string& text, int code = 200);
    // 便捷方法：{"error": "..."}
    static Response error(const std::string& message, int code = 400);
};

using Handler = std::function<Response(const Request&)>;

class Server {
public:
    Server();
    ~Server();

    // 注册路由。path 必须精确匹配（不含查询串）。
    void route(const std::string& method, const std::string& path, Handler handler);

    // 启动监听并阻塞处理请求，直到 stop() 被调用。
    // host 默认 127.0.0.1；返回 false 时 error 给出原因。
    bool listen(const std::string& host, int port, std::string& error);

    // 从另一个线程停止服务
    void stop();

    // 监听端口（listen 成功后有效）
    int port() const { return port_; }

public:
    // 内部实现细节（供 .cpp 里的自由函数访问）
    struct Impl;
private:
    Impl* impl_;
    int port_ = 0;
};

// ---- 工具函数 ----

// URL 解码（%XX 与 +）
std::string urlDecode(const std::string& s);
// URL 编码（除 unreserved 外全部转义）
std::string urlEncode(const std::string& s);
// 解析 a=1&b=2
std::map<std::string, std::string> parseQuery(const std::string& qs);

// ---- JSON 构建 ----
// 用于拼装响应，避免手工转义出错
class Json {
public:
    Json& beginObject();
    Json& endObject();
    Json& beginArray();
    Json& endArray();

    Json& key(const std::string& k);
    Json& value(const std::string& v);
    Json& value(const char* v);
    Json& value(long long v);
    Json& value(int v);
    Json& value(double v);
    Json& value(bool v);

    // 直接塞入已序列化的 JSON 片段（调用方保证合法）
    Json& raw(const std::string& jsonFragment);

    std::string str() const { return out_; }

private:
    void separator();
    std::string out_;
    std::vector<bool> firstStack_;   // true 表示该层还没有元素
    bool expectValue_ = false;
};

// 字符串 JSON 转义（不含外层引号）
std::string jsonEscape(const std::string& s);

} // namespace api

#endif
