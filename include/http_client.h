#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <string>
#include <map>

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    
    std::string get(const std::string& url, const std::map<std::string, std::string>& headers = {});
    std::string post(const std::string& url, const std::string& data, const std::map<std::string, std::string>& headers = {});
    
    void setUserAgent(const std::string& ua);
    void setTimeout(int seconds);
    
private:
    std::string userAgent;
    int timeout;
};

#endif
