#ifndef MCP_CLIENT_H
#define MCP_CLIENT_H

#include <string>
#include <vector>
#include <map>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

class McpClient {
public:
    McpClient(const std::string& mcpScriptPath);
    ~McpClient();
    
    bool initialize();
    std::string callTool(const std::string& toolName, const std::string& argsJson);
    void close();
    
    bool isConnected() const { return connected; }
    
private:
    std::string readLine();
    bool writeLine(const std::string& line);
    std::string extractContentText(const std::string& jsonResponse);
    
#ifdef _WIN32
    HANDLE hChildStdinRd, hChildStdinWr;
    HANDLE hChildStdoutRd, hChildStdoutWr;
    PROCESS_INFORMATION piProcInfo;
#else
    int stdinPipe[2];
    int stdoutPipe[2];
    pid_t childPid;
#endif
    
    std::string mcpScriptPath;
    int requestId;
    bool connected;
};

#endif
