#include "mcp_client.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

McpClient::McpClient(const std::string& mcpScriptPath) 
    : mcpScriptPath(mcpScriptPath), requestId(1), connected(false) {
#ifdef _WIN32
    hChildStdinRd = NULL; hChildStdinWr = NULL;
    hChildStdoutRd = NULL; hChildStdoutWr = NULL;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
#endif
}

McpClient::~McpClient() {
    close();
}

bool McpClient::initialize() {
#ifdef _WIN32
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;
    
    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0)) {
        return false;
    }
    if (!SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0)) {
        return false;
    }
    
    if (!CreatePipe(&hChildStdinRd, &hChildStdinWr, &saAttr, 0)) {
        return false;
    }
    if (!SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0)) {
        return false;
    }
    
    STARTUPINFO siStartInfo;
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    siStartInfo.hStdOutput = hChildStdoutWr;
    siStartInfo.hStdInput = hChildStdinRd;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;
    
    // 创建隐藏窗口
    siStartInfo.dwFlags |= STARTF_USESHOWWINDOW;
    siStartInfo.wShowWindow = SW_HIDE;
    
    std::string cmdLineStr = "node \"" + mcpScriptPath + "\"";
    std::vector<char> cmdLine(cmdLineStr.begin(), cmdLineStr.end());
    cmdLine.push_back('\0');
    
    if (!CreateProcess(NULL, cmdLine.data(), NULL, NULL, TRUE, 
                       CREATE_NO_WINDOW, NULL, NULL, &siStartInfo, &piProcInfo)) {
        return false;
    }
    
    CloseHandle(hChildStdoutWr);
    CloseHandle(hChildStdinRd);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 发送 initialize
    std::string initMsg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"ebook-reader\",\"version\":\"1.0\"}}}\n";
    writeLine(initMsg);
    
    // 读取响应
    std::string resp = readLine();
    if (resp.empty()) {
        return false;
    }
    
    // 发送 initialized 通知
    std::string initNotif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}\n";
    writeLine(initNotif);
    
    connected = true;
    requestId = 1;
    return true;
#else
    // Linux/macOS 实现
    if (pipe(stdinPipe) < 0 || pipe(stdoutPipe) < 0) {
        return false;
    }
    
    childPid = fork();
    if (childPid == 0) {
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdinPipe[0]); close(stdinPipe[1]);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        execlp("node", "node", mcpScriptPath.c_str(), (char*)NULL);
        exit(1);
    }
    
    close(stdinPipe[0]);
    close(stdoutPipe[1]);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    std::string initMsg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"ebook-reader\",\"version\":\"1.0\"}}}\n";
    writeLine(initMsg);
    
    std::string resp = readLine();
    if (resp.empty()) return false;
    
    std::string initNotif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}\n";
    writeLine(initNotif);
    
    connected = true;
    requestId = 1;
    return true;
#endif
}

bool McpClient::writeLine(const std::string& line) {
#ifdef _WIN32
    DWORD written;
    return WriteFile(hChildStdinWr, line.c_str(), line.length(), &written, NULL) && written == line.length();
#else
    return write(stdinPipe[1], line.c_str(), line.length()) == (ssize_t)line.length();
#endif
}

std::string McpClient::readLine() {
#ifdef _WIN32
    char buffer[4096];
    std::string result;
    DWORD read;
    while (true) {
        if (!ReadFile(hChildStdoutRd, buffer, 1, &read, NULL) || read == 0) {
            break;
        }
        if (buffer[0] == '\n') break;
        if (buffer[0] != '\r') {
            result += buffer[0];
        }
    }
    return result;
#else
    char buffer[4096];
    std::string result;
    while (true) {
        ssize_t n = ::read(stdoutPipe[0], buffer, 1);
        if (n <= 0) break;
        if (buffer[0] == '\n') break;
        if (buffer[0] != '\r') {
            result += buffer[0];
        }
    }
    return result;
#endif
}

std::string McpClient::callTool(const std::string& toolName, const std::string& argsJson) {
    if (!connected) return "";
    
    requestId++;
    std::string msg = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(requestId) + 
                      ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + toolName + 
                      "\",\"arguments\":" + argsJson + "}}\n";
    
    if (!writeLine(msg)) return "";
    
    for (int i = 0; i < 100; i++) {
        std::string line = readLine();
        if (line.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // 简单检查是否是响应
        if (line.find("\"id\":" + std::to_string(requestId)) != std::string::npos) {
            return extractContentText(line);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    return "";
}

std::string McpClient::extractContentText(const std::string& jsonResponse) {
    // 简单提取 content[0].text
    size_t pos = jsonResponse.find("\"text\":\"");
    if (pos == std::string::npos) {
        pos = jsonResponse.find("\"text\": \"");
    }
    if (pos == std::string::npos) return jsonResponse;
    
    size_t start = jsonResponse.find("\"", pos + 7);
    if (start == std::string::npos) start = jsonResponse.find(":", pos) + 1;
    start = jsonResponse.find("\"", start);
    if (start == std::string::npos) return "";
    start++;
    
    size_t end = start;
    while (end < jsonResponse.size()) {
        if (jsonResponse[end] == '"' && (end == 0 || jsonResponse[end-1] != '\\')) {
            break;
        }
        end++;
    }
    
    return jsonResponse.substr(start, end - start);
}

void McpClient::close() {
    if (!connected) return;
    connected = false;
    
#ifdef _WIN32
    if (piProcInfo.hProcess) {
        TerminateProcess(piProcInfo.hProcess, 0);
        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
    }
    if (hChildStdinWr) CloseHandle(hChildStdinWr);
    if (hChildStdoutRd) CloseHandle(hChildStdoutRd);
#else
    if (childPid > 0) {
        kill(childPid, SIGTERM);
        waitpid(childPid, NULL, 0);
    }
    ::close(stdinPipe[1]);
    ::close(stdoutPipe[0]);
#endif
}
