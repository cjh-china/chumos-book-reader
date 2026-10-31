#include "http_client.h"

#ifdef HAS_CURL
#include <curl/curl.h>
#include <sstream>
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

std::string HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
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
        if (res != CURLE_OK) {
            response = "";
        }

        if (headerList) {
            curl_slist_free_all(headerList);
        }
        curl_easy_cleanup(curl);
    }

    return response;
}

std::string HttpClient::post(const std::string& url, const std::string& data, const std::map<std::string, std::string>& headers) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)data.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
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
        if (res != CURLE_OK) {
            response = "";
        }

        if (headerList) {
            curl_slist_free_all(headerList);
        }
        curl_easy_cleanup(curl);
    }

    return response;
}

void HttpClient::setUserAgent(const std::string& ua) {
    userAgent = ua;
}

void HttpClient::setTimeout(int seconds) {
    timeout = seconds;
}

#else

HttpClient::HttpClient() : userAgent("Mozilla/5.0"), timeout(30) {}
HttpClient::~HttpClient() {}
std::string HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
    return "";
}
std::string HttpClient::post(const std::string& url, const std::string& data, const std::map<std::string, std::string>& headers) {
    return "";
}
void HttpClient::setUserAgent(const std::string& ua) {}
void HttpClient::setTimeout(int seconds) {}

#endif
