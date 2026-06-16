#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace tracymcp {
using json = nlohmann::json;

// JSON schema list for MCP tools/list.
json ToolList();

// Dispatch one tools/call. Returns an MCP result object
// ({"content":[...], "isError":bool}). Throws std::runtime_error
// (message is shown to the model) on bad input or load failure.
json CallTool(const std::string& name, const json& args);
}
