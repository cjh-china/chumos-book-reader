#ifndef MCP_POOL_H
#define MCP_POOL_H

#include "mcp_client.h"
#include <queue>
#include <mutex>
#include <memory>

class McpPool {
public:
    McpPool(const std::string& mcpScriptPath, int size);
    ~McpPool();
    
    std::string callTool(const std::string& toolName, const std::string& argsJson);
    void close();
    
private:
    std::string mcpScriptPath;
    std::queue<McpClient*> clients;
    std::mutex mutex;
    int poolSize;
};

#endif
