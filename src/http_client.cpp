#include "http_client.h"

#ifdef HAS_CURL
#include <curl/curl.h>
#include <cstdio>
#include <mutex>

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

static std::once_flag g_curlInitFlag;

HttpClient::HttpClient() : userAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"), timeout(30) {
    std::call_once(g_curlInitFlag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

HttpClient::~HttpClient() {
    // 不在每个实例析构时调用 curl_global_cleanup：
    // 该函数非线程安全，且并发下载时其他线程仍在使用 curl。
}

void HttpClient::recordFailure(const char* method, const std::string& url, int curlResult, long httpCode) {
    lastStatus_ = httpCode;
    if (curlResult != 0) {
        lastError_ = std::string(curl_easy_strerror((CURLcode)curlResult)) + " (" + method + " " + url + ")";
    } else if (httpCode >= 400) {
        lastError_ = "HTTP " + std::to_string(httpCode) + " (" + method + " " + url + ")";
    } else if (httpCode == 0) {
        lastError_ = std::string("无响应 (") + method + " " + url + ")";
    } else {
        lastError_.clear();
    }
}

std::string HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
    lastError_.clear();
    lastStatus_ = 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = "curl 初始化失败";
        return "";
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    // 调用方显式传了 User-Agent 就不再设 CURLOPT_USERAGENT，
    // 否则 curl 会发两个 UA 头，部分服务端直接拒绝
    if (headers.find("User-Agent") == headers.end() &&
        headers.find("user-agent") == headers.end()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    struct curl_slist* headerList = NULL;
    for (const auto& h : headers) {
        std::string headerStr = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, headerStr.c_str());
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    if (res != CURLE_OK || httpCode >= 400) {
        recordFailure("GET", url, (int)res, httpCode);
        response.clear();
    } else {
        lastStatus_ = httpCode;
    }

    if (headerList) {
        curl_slist_free_all(headerList);
    }
    curl_easy_cleanup(curl);

    return response;
}

std::string HttpClient::post(const std::string& url, const std::string& data, const std::map<std::string, std::string>& headers) {
    lastError_.clear();
    lastStatus_ = 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = "curl 初始化失败";
        return "";
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)data.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (headers.find("User-Agent") == headers.end() &&
        headers.find("user-agent") == headers.end()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    struct curl_slist* headerList = NULL;
    for (const auto& h : headers) {
        std::string headerStr = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, headerStr.c_str());
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    if (res != CURLE_OK || httpCode >= 400) {
        recordFailure("POST", url, (int)res, httpCode);
        response.clear();
    } else {
        lastStatus_ = httpCode;
    }

    if (headerList) {
        curl_slist_free_all(headerList);
    }
    curl_easy_cleanup(curl);

    return response;
}

void HttpClient::setUserAgent(const std::string& ua) {
    userAgent = ua;
}

void HttpClient::setTimeout(int seconds) {
    timeout = seconds;
}

#else

// 未编译 libcurl 时的降级实现。
// 必须显式记录错误，否则调用方只看到空串，无从定位。
HttpClient::HttpClient() : userAgent("Mozilla/5.0"), timeout(30) {}
HttpClient::~HttpClient() {}

void HttpClient::recordFailure(const char* method, const std::string& url, int curlResult, long httpCode) {
    (void)curlResult;
    (void)httpCode;
    lastStatus_ = 0;
    lastError_ = std::string("未编译 libcurl（HAS_CURL 未定义），无法发出 ") + method + " " + url;
}

std::string HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
    (void)headers;
    recordFailure("GET", url, 0, 0);
    return "";
}

std::string HttpClient::post(const std::string& url, const std::string& data, const std::map<std::string, std::string>& headers) {
    (void)data;
    (void)headers;
    recordFailure("POST", url, 0, 0);
    return "";
}

void HttpClient::setUserAgent(const std::string& ua) {}
void HttpClient::setTimeout(int seconds) {}

#endif
