// tracy-mcp: an MCP server over Tracy's .tracy captures.
// Transport: newline-delimited JSON-RPC 2.0 on stdin/stdout (MCP stdio).
// Logging goes to stderr only -- stdout is the protocol channel.

#include "tools.hpp"

#include <iostream>
#include <string>

using tracymcp::json;

namespace {

const char* kProtocol = "2025-06-18";

void Send(const json& msg) {
    std::cout << msg.dump() << "\n" << std::flush;
}

void Reply(const json& id, const json& result) {
    Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void ReplyError(const json& id, int code, const std::string& message) {
    Send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

void Handle(const json& req) {
    const std::string method = req.value("method", "");
    const bool isNotification = !req.contains("id") || req["id"].is_null();
    const json id = isNotification ? json(nullptr) : req["id"];
    const json params = req.value("params", json::object());

    if (method == "initialize") {
        std::string ver = params.value("protocolVersion", kProtocol);
        Reply(id, {
            {"protocolVersion", ver},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "tracy-mcp"}, {"version", "0.1.0"}}},
        });
        return;
    }
    if (method == "tools/list") {
        Reply(id, {{"tools", tracymcp::ToolList()}});
        return;
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        const json args = params.value("arguments", json::object());
        try {
            Reply(id, tracymcp::CallTool(name, args));
        } catch (const std::exception& e) {
            // MCP convention: tool failures are a normal result with isError=true,
            // so the model can read and react to the message.
            Reply(id, {
                {"content", json::array({{{"type", "text"}, {"text", std::string("Error: ") + e.what()}}})},
                {"isError", true},
            });
        }
        return;
    }
    if (method == "ping") {
        Reply(id, json::object());
        return;
    }
    if (isNotification) return;  // notifications/initialized, cancelled, etc.

    ReplyError(id, -32601, "method not found: " + method);
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            ReplyError(nullptr, -32700, std::string("parse error: ") + e.what());
            continue;
        }
        Handle(req);
    }
    return 0;
}
