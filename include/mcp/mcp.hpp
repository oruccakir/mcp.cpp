#pragma once

// Umbrella header for the MCP C++ SDK.

#include <mcp/capabilities.hpp>
#include <mcp/client/client.hpp>
#include <mcp/client/elicitation.hpp>
#include <mcp/client/roots_provider.hpp>
#include <mcp/client/sampling.hpp>
#include <mcp/content.hpp>
#include <mcp/core/cancellation.hpp>
#include <mcp/core/progress.hpp>
#include <mcp/core/router.hpp>
#include <mcp/core/session.hpp>
#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/jsonrpc/message.hpp>
#include <mcp/methods.hpp>
#include <mcp/result.hpp>
#include <mcp/server/completion.hpp>
#include <mcp/server/logging.hpp>
#include <mcp/server/prompt_provider.hpp>
#include <mcp/server/resource_provider.hpp>
#include <mcp/server/server.hpp>
#include <mcp/server/tool_registry.hpp>
#include <mcp/transport/stdio_client_transport.hpp>
#include <mcp/transport/stdio_transport.hpp>
#include <mcp/transport/transport.hpp>
#include <mcp/types.hpp>
