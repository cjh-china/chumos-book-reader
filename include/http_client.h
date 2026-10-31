#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <string>
#include <map>

// 轻量 HTTP 客户端（基于 libcurl）。
// 失败时 get/post 返回空串，可通过 lastError() 获取具体原因
// （curl 错误描述 / HTTP 状态码 / 未编译 libcurl）。
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    std::string get(const std::string& url, const std::map<std::string, std::string>& headers = {});
    std::string post(const std::string& url, const std::string& data, const std::map<std::string, std::string>& headers = {});

    void setUserAgent(const std::string& ua);
    void setTimeout(int seconds);

    // 最近一次 get/post 的失败原因；成功后为空
    const std::string& lastError() const { return lastError_; }
    // 最近一次响应的 HTTP 状态码；未拿到响应时为 0
    long lastStatus() const { return lastStatus_; }

private:
    // 记录一次失败：curlResult 为 0（CURLE_OK）表示传输成功，此时只看 HTTP 状态
    void recordFailure(const char* method, const std::string& url, int curlResult, long httpCode);

    std::string userAgent;
    int timeout;
    std::string lastError_;
    long lastStatus_ = 0;
};

#endif
