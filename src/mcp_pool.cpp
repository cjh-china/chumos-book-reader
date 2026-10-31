#include "mcp_pool.h"
#include <thread>
#include <chrono>

McpPool::McpPool(const std::string& mcpScriptPath, int size)
    : mcpScriptPath(mcpScriptPath), poolSize(size) {
    for (int i = 0; i < size; i++) {
        McpClient* client = new McpClient(mcpScriptPath);
        if (client->initialize()) {
            clients.push(client);
        } else {
            delete client;
        }
    }
}

McpPool::~McpPool() {
    close();
}

std::string McpPool::callTool(const std::string& toolName, const std::string& argsJson) {
    McpClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (clients.empty()) {
            McpClient* nc = new McpClient(mcpScriptPath);
            if (!nc->initialize()) {
                delete nc;
                return "";
            }
            clients.push(nc);
        }
        client = clients.front();
        clients.pop();
    }

    std::string result = client->callTool(toolName, argsJson);

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (client->isConnected()) {
            clients.push(client);
        } else {
            delete client;
            McpClient* newClient = new McpClient(mcpScriptPath);
            if (newClient->initialize()) {
                clients.push(newClient);
            } else {
                delete newClient;
            }
        }
    }

    return result;
}

void McpPool::close() {
    std::lock_guard<std::mutex> lock(mutex);
    while (!clients.empty()) {
        McpClient* client = clients.front();
        clients.pop();
        client->close();
        delete client;
    }
}
